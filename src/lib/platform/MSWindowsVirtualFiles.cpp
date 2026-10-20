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

#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr UINT kSetFilesMessage = WM_APP + 1;
constexpr auto kSelfTestText = "deskflow-selftest:virtual-file";
constexpr auto kSelfTestTextOpen = "deskflow-selftest:virtual-file:open";

struct SetFilesRequest
{
  std::vector<MSWindowsVirtualFiles::File> files;
  MSWindowsVirtualFiles::ComAccess access;
  std::promise<bool> done;
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

// COM security is process-wide and can only be set once, before anything is marshalled.
void initComSecurity(MSWindowsVirtualFiles::ComAccess access)
{
  static std::once_flag once;
  std::call_once(once, [access] {
    HRESULT hr = E_FAIL;
    if (access == MSWindowsVirtualFiles::ComAccess::Anyone) {
      hr = CoInitializeSecurity(
          nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr
      );
      LOG_INFO("virtual files: COM access for anyone, result=0x%08lx", static_cast<unsigned long>(hr));
      return;
    }

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

    hr = CoInitializeSecurity(
        sd.data(), -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr
    );
    LOG_INFO("virtual files: COM access for interactive users, result=0x%08lx", static_cast<unsigned long>(hr));
  });
}

//! The clipboard object Explorer reads files from when the user pastes
class VirtualFileDataObject : public IDataObject
{
public:
  explicit VirtualFileDataObject(std::vector<MSWindowsVirtualFiles::File> files) : m_files(std::move(files))
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
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
  {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *object = static_cast<IDataObject *>(this);
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
    LOG_INFO(
        "virtual files: GetData format=%s index=%ld tymed=0x%lx", formatName(format->cfFormat).c_str(), format->lindex,
        static_cast<unsigned long>(format->tymed)
    );

    if (format->cfFormat == m_descriptorFormat && (format->tymed & TYMED_HGLOBAL)) {
      return getDescriptor(medium);
    }
    if (format->cfFormat == m_contentsFormat && (format->tymed & TYMED_ISTREAM)) {
      return getContents(format->lindex, medium);
    }
    if (format->cfFormat == m_dropEffectFormat && (format->tymed & TYMED_HGLOBAL)) {
      return putDword(DROPEFFECT_COPY, medium);
    }
    if ((format->cfFormat == m_noHistoryFormat || format->cfFormat == m_noCloudFormat) &&
        (format->tymed & TYMED_HGLOBAL)) {
      return putDword(0, medium);
    }
    if ((format->cfFormat == m_ownershipFormat || format->cfFormat == m_noMonitorFormat) &&
        (format->tymed & TYMED_HGLOBAL)) {
      return putDword(0, medium);
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
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release) override
  {
    if (format != nullptr) {
      LOG_INFO("virtual files: SetData format=%s", formatName(format->cfFormat).c_str());
    }
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

private:
  virtual ~VirtualFileDataObject() = default;

  HRESULT getDescriptor(STGMEDIUM *medium) const
  {
    const auto count = m_files.size();
    const SIZE_T size = sizeof(FILEGROUPDESCRIPTORW) + (count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
    HGLOBAL global = GlobalAlloc(GHND, size);
    if (global == nullptr) {
      return E_OUTOFMEMORY;
    }

    auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(global));
    group->cItems = static_cast<UINT>(count);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    for (size_t i = 0; i < count; ++i) {
      FILEDESCRIPTORW &file = group->fgd[i];
      file.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI | FD_UNICODE;
      file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      file.ftLastWriteTime = now;
      const auto fileSize = static_cast<unsigned long long>(m_files[i].content.size());
      file.nFileSizeHigh = static_cast<DWORD>(fileSize >> 32);
      file.nFileSizeLow = static_cast<DWORD>(fileSize & 0xffffffffu);
      wcsncpy_s(file.cFileName, MAX_PATH, m_files[i].name.c_str(), _TRUNCATE);
    }
    GlobalUnlock(global);

    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = global;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  HRESULT getContents(LONG index, STGMEDIUM *medium) const
  {
    if (index < 0 || static_cast<size_t>(index) >= m_files.size()) {
      return DV_E_LINDEX;
    }
    const auto &file = m_files[static_cast<size_t>(index)];
    IStream *stream =
        SHCreateMemStream(reinterpret_cast<const BYTE *>(file.content.data()), static_cast<UINT>(file.content.size()));
    if (stream == nullptr) {
      return E_OUTOFMEMORY;
    }
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = stream;
    medium->pUnkForRelease = nullptr;
    LOG_INFO("virtual files: serving %s (%zu bytes)", narrow(file.name).c_str(), file.content.size());
    return S_OK;
  }

  static HRESULT putDword(DWORD value, STGMEDIUM *medium)
  {
    HGLOBAL global = GlobalAlloc(GHND, sizeof(DWORD));
    if (global == nullptr) {
      return E_OUTOFMEMORY;
    }
    *static_cast<DWORD *>(GlobalLock(global)) = value;
    GlobalUnlock(global);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = global;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  std::atomic<ULONG> m_refs{1};
  std::vector<MSWindowsVirtualFiles::File> m_files;
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

bool MSWindowsVirtualFiles::isSelfTestRequest(const std::string &text, ComAccess &access)
{
  if (text == kSelfTestTextOpen) {
    access = ComAccess::Anyone;
    return true;
  }
  if (text == kSelfTestText) {
    access = ComAccess::InteractiveUsers;
    return true;
  }
  return false;
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
      initComSecurity(request->access);

      auto *object = new VirtualFileDataObject(std::move(request->files));
      const HRESULT hr = OleSetClipboard(object);
      object->Release();
      LOG_INFO("virtual files: clipboard set, result=0x%08lx", static_cast<unsigned long>(hr));
      request->done.set_value(SUCCEEDED(hr));
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  OleUninitialize();
}

bool MSWindowsVirtualFiles::setFiles(std::vector<File> files, ComAccess access)
{
  if (!start()) {
    LOG_ERR("virtual files: failed to start the clipboard thread");
    return false;
  }

  auto *request = new SetFilesRequest{std::move(files), access, {}};
  auto done = request->done.get_future();
  if (!PostThreadMessageW(m_threadId, kSetFilesMessage, 0, reinterpret_cast<LPARAM>(request))) {
    LOG_ERR("virtual files: failed to reach the clipboard thread, error=%lu", GetLastError());
    delete request;
    return false;
  }

  if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    LOG_WARN("virtual files: timed out setting the clipboard");
    return false;
  }
  return done.get();
}
