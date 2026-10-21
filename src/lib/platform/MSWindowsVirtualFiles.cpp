/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsVirtualFiles.h"

#include "base/Log.h"

#include <Windows.h>

#include <ole2.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <mutex>
#include <thread>

namespace {

constexpr UINT kSetFilesMessage = WM_APP + 1;
constexpr auto kSelfTestText = "deskflow-selftest:virtual-file";
constexpr int64_t kUnixToFileTimeSeconds = 11644473600LL;

struct SetFilesRequest
{
  std::shared_ptr<VirtualFileSource> source;
  std::wstring text;
};

std::string narrow(const std::wstring &text)
{
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), size, nullptr, nullptr);
  return result;
}

std::string formatName(CLIPFORMAT format)
{
  wchar_t name[128] = {};
  if (GetClipboardFormatNameW(format, name, 127) > 0) {
    return narrow(name);
  }
  return std::to_string(format);
}

FILETIME toFileTime(int64_t unixSeconds)
{
  const auto ticks = static_cast<uint64_t>(std::max<int64_t>(unixSeconds + kUnixToFileTimeSeconds, 0)) * 10000000ULL;
  FILETIME time;
  time.dwLowDateTime = static_cast<DWORD>(ticks & 0xffffffffu);
  time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return time;
}

HRESULT putGlobal(const void *data, size_t size, STGMEDIUM *medium)
{
  HGLOBAL global = GlobalAlloc(GHND, std::max<size_t>(size, 1));
  if (global == nullptr) {
    return E_OUTOFMEMORY;
  }
  if (size > 0) {
    void *target = GlobalLock(global);
    if (target == nullptr) {
      GlobalFree(global);
      return E_OUTOFMEMORY;
    }
    memcpy(target, data, size);
    GlobalUnlock(global);
  }
  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = global;
  medium->pUnkForRelease = nullptr;
  return S_OK;
}

// COM security is process-wide and can only be set once, before anything is marshalled.
void initComSecurity()
{
  static std::once_flag once;
  std::call_once(once, [] {
    // COM wants an absolute security descriptor, which also has to stay valid while the process runs.
    // Explorer runs as the signed-in user at medium integrity, so allow interactive users at that level.
    PSECURITY_DESCRIPTOR relative = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:(A;;0x3;;;SY)(A;;0x3;;;BA)(A;;0x3;;;IU)S:(ML;;NX;;;ME)", SDDL_REVISION_1, &relative, nullptr
        )) {
      LOG_WARN("virtual files: invalid COM security descriptor, error=%lu", GetLastError());
      return;
    }

    DWORD sdSize = 0, daclSize = 0, saclSize = 0, ownerSize = 0, groupSize = 0;
    MakeAbsoluteSD(relative, nullptr, &sdSize, nullptr, &daclSize, nullptr, &saclSize, nullptr, &ownerSize, nullptr, &groupSize);
    static std::vector<BYTE> sd, dacl, sacl, owner, group;
    sd.resize(sdSize);
    dacl.resize(daclSize);
    sacl.resize(saclSize);
    owner.resize(ownerSize);
    group.resize(groupSize);
    const BOOL made = MakeAbsoluteSD(
        relative, sd.data(), &sdSize, reinterpret_cast<PACL>(dacl.data()), &daclSize,
        reinterpret_cast<PACL>(sacl.data()), &saclSize, owner.data(), &ownerSize, group.data(), &groupSize
    );
    LocalFree(relative);
    if (!made) {
      LOG_WARN("virtual files: failed to make an absolute security descriptor, error=%lu", GetLastError());
      return;
    }

    const HRESULT hr = CoInitializeSecurity(
        sd.data(), -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr
    );
    if (FAILED(hr)) {
      // e.g. RPC_E_TOO_LATE: Explorer may then be refused when it pastes files from the other computer
      LOG_WARN("virtual files: couldn't set COM access, result=0x%08lx", static_cast<unsigned long>(hr));
    } else {
      LOG_DEBUG("virtual files: COM access for interactive users");
    }
  });
}

//! Files held in memory, for the test hook
class MemorySource : public VirtualFileSource
{
public:
  explicit MemorySource(std::vector<std::pair<std::wstring, std::string>> files) : m_files(std::move(files))
  {
  }

  bool describe(std::vector<VirtualEntry> &entries, std::string &) override
  {
    for (const auto &[name, content] : m_files) {
      entries.push_back({name, false, content.size(), static_cast<int64_t>(time(nullptr))});
    }
    return true;
  }

  std::unique_ptr<VirtualReader> open(size_t index, uint64_t offset, std::string &error) override
  {
    if (index >= m_files.size()) {
      error = "no such file";
      return nullptr;
    }
    return std::make_unique<Reader>(m_files[index].second, offset);
  }

private:
  class Reader : public VirtualReader
  {
  public:
    Reader(const std::string &content, uint64_t offset) : m_content(content), m_offset(offset)
    {
    }

    bool read(char *buffer, size_t size, size_t &count) override
    {
      count = 0;
      if (m_offset < m_content.size()) {
        count = static_cast<size_t>(std::min<uint64_t>(size, m_content.size() - m_offset));
        std::copy_n(m_content.data() + m_offset, count, buffer);
        m_offset += count;
      }
      return true;
    }

  private:
    const std::string &m_content;
    uint64_t m_offset;
  };

  std::vector<std::pair<std::wstring, std::string>> m_files;
};

//! One file's content, read from its source as Explorer asks for it
class VirtualFileStream : public IStream
{
public:
  VirtualFileStream(std::shared_ptr<VirtualFileSource> source, size_t index, VirtualEntry entry)
      : m_source(std::move(source)),
        m_index(index),
        m_entry(std::move(entry))
  {
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
  {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
      *object = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++m_refs;
  }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG refs = --m_refs;
    if (refs == 0) {
      delete this;
    }
    return refs;
  }

  // ISequentialStream
  HRESULT STDMETHODCALLTYPE Read(void *buffer, ULONG size, ULONG *read) override
  {
    if (read != nullptr) {
      *read = 0;
    }
    if (buffer == nullptr) {
      return STG_E_INVALIDPOINTER;
    }
    if (m_reader == nullptr) {
      std::string error;
      m_reader = m_source->open(m_index, m_position, error);
      if (m_reader == nullptr) {
        LOG_WARN("virtual files: can't read %s: %s", narrow(m_entry.path).c_str(), error.c_str());
        return STG_E_READFAULT;
      }
    }

    size_t count = 0;
    if (!m_reader->read(static_cast<char *>(buffer), size, count)) {
      LOG_WARN("virtual files: reading %s failed", narrow(m_entry.path).c_str());
      m_reader.reset();
      return STG_E_READFAULT;
    }
    m_position += count;
    if (read != nullptr) {
      *read = static_cast<ULONG>(count);
    }
    return count == size ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override
  {
    return STG_E_ACCESSDENIED;
  }

  // IStream
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPosition) override
  {
    int64_t base = 0;
    switch (origin) {
    case STREAM_SEEK_SET:
      base = 0;
      break;
    case STREAM_SEEK_CUR:
      base = static_cast<int64_t>(m_position);
      break;
    case STREAM_SEEK_END:
      base = static_cast<int64_t>(m_entry.size);
      break;
    default:
      return STG_E_INVALIDFUNCTION;
    }
    const int64_t target = base + move.QuadPart;
    if (target < 0) {
      return STG_E_INVALIDFUNCTION;
    }
    if (static_cast<uint64_t>(target) != m_position) {
      // reopened at the new position on the next read
      m_reader.reset();
      m_position = static_cast<uint64_t>(target);
    }
    if (newPosition != nullptr) {
      newPosition->QuadPart = m_position;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
  {
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) override
  {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Commit(DWORD) override
  {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Revert() override
  {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
  {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
  {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD flags) override
  {
    if (stat == nullptr) {
      return STG_E_INVALIDPOINTER;
    }
    ZeroMemory(stat, sizeof(*stat));
    if ((flags & STATFLAG_NONAME) == 0) {
      const auto name = m_entry.path.substr(m_entry.path.find_last_of(L'\\') + 1);
      const auto bytes = (name.size() + 1) * sizeof(wchar_t);
      stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
      if (stat->pwcsName != nullptr) {
        memcpy(stat->pwcsName, name.c_str(), bytes);
      }
    }
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = m_entry.size;
    stat->mtime = toFileTime(m_entry.modified);
    stat->grfMode = STGM_READ;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream **) override
  {
    return E_NOTIMPL;
  }

private:
  virtual ~VirtualFileStream() = default;

  std::atomic<ULONG> m_refs{1};
  std::shared_ptr<VirtualFileSource> m_source;
  size_t m_index;
  VirtualEntry m_entry;
  std::unique_ptr<VirtualReader> m_reader;
  uint64_t m_position = 0;
};

//! The clipboard object Explorer reads the paste from
class VirtualFileDataObject : public IDataObject, public IDataObjectAsyncCapability
{
public:
  VirtualFileDataObject(std::shared_ptr<VirtualFileSource> source, std::wstring text)
      : m_source(std::move(source)),
        m_text(std::move(text))
  {
    m_descriptorFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    m_contentsFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    m_dropEffectFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect"));
    m_ownershipFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Deskflow Ownership"));
    m_noMonitorFormat =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"));
    m_noHistoryFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"));
    m_noCloudFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"CanUploadToCloudClipboard"));

    m_formats = {
        {m_descriptorFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {m_contentsFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
        {m_dropEffectFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {m_ownershipFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {m_noMonitorFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {m_noHistoryFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {m_noCloudFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
    };
    if (!m_text.empty()) {
      m_formats.push_back({CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    }
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
  {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *object = static_cast<IDataObject *>(this);
    } else if (riid == IID_IDataObjectAsyncCapability) {
      *object = static_cast<IDataObjectAsyncCapability *>(this);
    } else {
      *object = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++m_refs;
  }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG refs = --m_refs;
    if (refs == 0) {
      LOG_DEBUG("virtual files: clipboard object released");
      delete this;
    }
    return refs;
  }

  // IDataObject
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
  {
    if (format == nullptr || medium == nullptr) {
      return E_INVALIDARG;
    }
    ZeroMemory(medium, sizeof(*medium));
    LOG_DEBUG(
        "virtual files: GetData format=%s index=%ld tymed=0x%lx", formatName(format->cfFormat).c_str(), format->lindex,
        static_cast<unsigned long>(format->tymed)
    );

    if (format->cfFormat == m_descriptorFormat && (format->tymed & TYMED_HGLOBAL)) {
      return getDescriptor(medium);
    }
    if (format->cfFormat == m_contentsFormat && (format->tymed & TYMED_ISTREAM)) {
      return getContents(format->lindex, medium);
    }
    if (format->cfFormat == CF_UNICODETEXT && !m_text.empty() && (format->tymed & TYMED_HGLOBAL)) {
      return putGlobal(m_text.c_str(), (m_text.size() + 1) * sizeof(wchar_t), medium);
    }
    if (format->tymed & TYMED_HGLOBAL) {
      DWORD value = 0;
      if (format->cfFormat == m_dropEffectFormat) {
        value = DROPEFFECT_COPY;
      } else if (format->cfFormat != m_noHistoryFormat && format->cfFormat != m_noCloudFormat &&
                 format->cfFormat != m_ownershipFormat && format->cfFormat != m_noMonitorFormat) {
        return DV_E_FORMATETC;
      }
      return putGlobal(&value, sizeof(value), medium);
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override
  {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
  {
    if (format == nullptr) {
      return E_INVALIDARG;
    }
    for (const auto &offered : m_formats) {
      if (offered.cfFormat == format->cfFormat) {
        return (offered.tymed & format->tymed) ? S_OK : DV_E_TYMED;
      }
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override
  {
    if (out != nullptr) {
      out->ptd = nullptr;
    }
    return E_NOTIMPL;
  }

  // Explorer reports what it did with the data (e.g. "Performed DropEffect"); nothing to keep
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *medium, BOOL release) override
  {
    if (release && medium != nullptr) {
      ReleaseStgMedium(medium);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **formats) override
  {
    if (formats == nullptr) {
      return E_INVALIDARG;
    }
    if (direction != DATADIR_GET) {
      *formats = nullptr;
      return E_NOTIMPL;
    }
    return SHCreateStdEnumFmtEtc(static_cast<UINT>(m_formats.size()), m_formats.data(), formats);
  }

  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  // IDataObjectAsyncCapability: lets Explorer do the paste on its own background thread
  HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL async) override
  {
    m_async = async != FALSE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *async) override
  {
    if (async == nullptr) {
      return E_INVALIDARG;
    }
    *async = m_async ? TRUE : FALSE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override
  {
    m_inOperation = true;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE InOperation(BOOL *inOperation) override
  {
    if (inOperation == nullptr) {
      return E_INVALIDARG;
    }
    *inOperation = m_inOperation ? TRUE : FALSE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx *, DWORD) override
  {
    m_inOperation = false;
    LOG_INFO("virtual files: paste finished, result=0x%08lx", static_cast<unsigned long>(result));
    return S_OK;
  }

private:
  virtual ~VirtualFileDataObject() = default;

  bool describe()
  {
    if (m_described) {
      return true;
    }
    std::string error;
    std::vector<VirtualEntry> entries;
    if (!m_source->describe(entries, error)) {
      LOG_WARN("virtual files: can't list the files to paste: %s", error.c_str());
      return false;
    }
    m_entries = std::move(entries);
    m_described = true;
    LOG_INFO("virtual files: pasting %zu files and folders", m_entries.size());
    return true;
  }

  HRESULT getDescriptor(STGMEDIUM *medium)
  {
    if (!describe()) {
      return E_FAIL;
    }
    const auto count = m_entries.size();
    const SIZE_T size = sizeof(FILEGROUPDESCRIPTORW) + (count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
    HGLOBAL global = GlobalAlloc(GHND, size);
    if (global == nullptr) {
      return E_OUTOFMEMORY;
    }

    auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(global));
    if (group == nullptr) {
      GlobalFree(global);
      return E_OUTOFMEMORY;
    }
    group->cItems = static_cast<UINT>(count);
    for (size_t i = 0; i < count; ++i) {
      const auto &entry = m_entries[i];
      FILEDESCRIPTORW &file = group->fgd[i];
      file.dwFlags = FD_ATTRIBUTES | FD_WRITESTIME | FD_UNICODE;
      file.ftLastWriteTime = toFileTime(entry.modified);
      if (entry.isDirectory) {
        file.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
      } else {
        file.dwFlags |= FD_FILESIZE | FD_PROGRESSUI;
        file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        file.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
        file.nFileSizeLow = static_cast<DWORD>(entry.size & 0xffffffffu);
      }
      wcsncpy_s(file.cFileName, MAX_PATH, entry.path.c_str(), _TRUNCATE);
    }
    GlobalUnlock(global);

    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = global;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  HRESULT getContents(LONG index, STGMEDIUM *medium)
  {
    if (!describe()) {
      return E_FAIL;
    }
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size() ||
        m_entries[static_cast<size_t>(index)].isDirectory) {
      return DV_E_LINDEX;
    }
    const auto i = static_cast<size_t>(index);
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = new VirtualFileStream(m_source, i, m_entries[i]);
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  std::atomic<ULONG> m_refs{1};
  std::shared_ptr<VirtualFileSource> m_source;
  std::wstring m_text;
  std::vector<VirtualEntry> m_entries;
  bool m_described = false;
  bool m_async = true;
  bool m_inOperation = false;
  std::vector<FORMATETC> m_formats;
  CLIPFORMAT m_descriptorFormat = 0;
  CLIPFORMAT m_contentsFormat = 0;
  CLIPFORMAT m_dropEffectFormat = 0;
  CLIPFORMAT m_ownershipFormat = 0;
  CLIPFORMAT m_noMonitorFormat = 0;
  CLIPFORMAT m_noHistoryFormat = 0;
  CLIPFORMAT m_noCloudFormat = 0;
};

} // namespace

MSWindowsVirtualFiles &MSWindowsVirtualFiles::instance()
{
  // never destroyed: the OLE thread lives as long as the process
  static auto *s_instance = new MSWindowsVirtualFiles();
  return *s_instance;
}

void MSWindowsVirtualFiles::initializeComSecurity()
{
  initComSecurity();
}

bool MSWindowsVirtualFiles::isSelfTestRequest(const std::string &text)
{
  return text == kSelfTestText;
}

IDataObject *MSWindowsVirtualFiles::createDataObject(std::shared_ptr<VirtualFileSource> source, const std::wstring &text)
{
  return static_cast<IDataObject *>(new VirtualFileDataObject(std::move(source), text));
}

std::shared_ptr<VirtualFileSource>
MSWindowsVirtualFiles::memorySource(std::vector<std::pair<std::wstring, std::string>> files)
{
  return std::make_shared<MemorySource>(std::move(files));
}

std::wstring MSWindowsVirtualFiles::windowsName(const std::wstring &name)
{
  // look-alikes for the characters Windows doesn't allow in names
  std::wstring result;
  for (const wchar_t c : name) {
    switch (c) {
    case L'<':
      result += L'\xFF1C';
      break;
    case L'>':
      result += L'\xFF1E';
      break;
    case L':':
      result += L'\xFF1A';
      break;
    case L'"':
      result += L'\xFF02';
      break;
    case L'/':
      result += L'\xFF0F';
      break;
    case L'\\':
      result += L'\xFF3C';
      break;
    case L'|':
      result += L'\xFF5C';
      break;
    case L'?':
      result += L'\xFF1F';
      break;
    case L'*':
      result += L'\xFF0A';
      break;
    default:
      result += c < 0x20 ? L'_' : c;
    }
  }

  // Windows drops trailing dots and spaces
  while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) {
    result.back() = L'_';
  }
  if (result.empty()) {
    return L"_";
  }

  // device names can't be used, whatever the extension
  static const wchar_t *const reserved[] = {L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3", L"COM4",
                                            L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3",
                                            L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
  const auto stem = result.substr(0, result.find(L'.'));
  for (const auto *device : reserved) {
    if (_wcsicmp(stem.c_str(), device) == 0) {
      result.insert(stem.size(), L"_");
      break;
    }
  }
  return result;
}

bool MSWindowsVirtualFiles::start()
{
  if (m_threadId != 0) {
    return true;
  }

  auto started = std::make_shared<std::promise<DWORD>>();
  auto threadId = started->get_future();
  std::thread([this, started] {
    // create this thread's message queue before anyone posts to it
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    started->set_value(GetCurrentThreadId());
    run();
  }).detach();

  m_threadId = threadId.get();
  return m_threadId != 0;
}

void MSWindowsVirtualFiles::run()
{
  const HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole)) {
    LOG_ERR("virtual files: OleInitialize failed, result=0x%08lx", static_cast<unsigned long>(ole));
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd == nullptr && msg.message == kSetFilesMessage) {
      std::unique_ptr<SetFilesRequest> request(reinterpret_cast<SetFilesRequest *>(msg.lParam));
      initComSecurity(); // done at startup already, unless this process skipped that

      auto *object = new VirtualFileDataObject(std::move(request->source), std::move(request->text));
      const HRESULT hr = OleSetClipboard(static_cast<IDataObject *>(object));
      object->Release();
      if (FAILED(hr)) {
        LOG_WARN("virtual files: couldn't set the clipboard, result=0x%08lx", static_cast<unsigned long>(hr));
      } else {
        LOG_DEBUG("virtual files: clipboard set");
      }
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  OleUninitialize();
}

bool MSWindowsVirtualFiles::setFiles(std::shared_ptr<VirtualFileSource> source, const std::wstring &text)
{
  if (!start()) {
    LOG_ERR("virtual files: failed to start the clipboard thread");
    return false;
  }

  // Never wait for the clipboard thread: taking over the clipboard sends a message to the previous
  // owner, which may be this thread's own window
  auto *request = new SetFilesRequest{std::move(source), text};
  if (!PostThreadMessageW(m_threadId, kSetFilesMessage, 0, reinterpret_cast<LPARAM>(request))) {
    LOG_ERR("virtual files: failed to reach the clipboard thread, error=%lu", GetLastError());
    delete request;
    return false;
  }
  return true;
}
