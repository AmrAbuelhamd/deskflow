# Deskflow with file copy

A fork of [Deskflow](https://github.com/deskflow/deskflow), the free and open-source app that lets one keyboard and mouse control several computers. It follows Deskflow's releases and adds:

- **Copy and paste files between computers.** Copy files or folders in Finder and paste them in Explorer, or the other way round, with the usual ⌘C/Ctrl+C and ⌘V/Ctrl+V. Copying sends only names and sizes; the content moves when you paste, over its own encrypted connection, so the pointer never waits for it.
- **No pointer freeze after copying something big.** A large clipboard, such as a big image, is sent in the background instead of holding up the keyboard and mouse.

Everything else works as in Deskflow; see [Deskflow's README](https://github.com/deskflow/deskflow#readme) for setting it up.

## Download

The [latest release](https://github.com/AmrAbuelhamd/deskflow/releases/latest) has:

| Computer | File |
|---|---|
| Mac with Apple silicon | `deskflow-<version>-macos-arm64.dmg` |
| Windows (x64) | `deskflow-<version>-win-x64.msi`, or the portable `.zip` |

Releases are named after the Deskflow release they're built on, plus `-amr.<n>`, e.g. `1.27.0-amr.4`.

Apple and Microsoft haven't signed these builds, so the first install asks for confirmation:

- **Mac:** if macOS can't check Deskflow for malware, open System Settings → Privacy & Security and click **Open Anyway**. Then allow Deskflow under **Accessibility**, as any Deskflow needs.
- **Windows:** if SmartScreen says it protected your PC, click **More info**, then **Run anyway**.

## Copying files

Install this fork on each computer you want to copy files between, and keep TLS on, as it is by default. Computers running stock Deskflow still work with it, just without file copying.

- **Mac to Windows:** Explorer reads the files from the Mac while it pastes, with its usual progress window and Cancel button.
- **Windows to Mac:** files in a Windows shared folder that the Mac has mounted are copied by Finder straight from the share; macOS asks once whether Deskflow may access network volumes. Other files are downloaded when you paste, and Finder waits until they're there, so items over about 10 GB are refused; copy those from a shared folder.
- Only files you've copied can be read, and they're read as the person signed in on the computer they come from.
- File content uses the port after Deskflow's own (24801 by default) on the server, so a firewall there has to allow it.

Tested with a Mac as the server and a Windows PC as the client. File copying isn't available on Linux.

Known limits on Windows: a file whose path inside a copied folder is 260 characters or longer is skipped, and two names that differ only in characters Windows doesn't allow can end up the same.

## Updating

Install a newer release over the old one; your settings stay. On a Mac, [`deskflow-update.sh`](https://github.com/AmrAbuelhamd/deskflow/blob/amr/main/.github/fork/deskflow-update.sh) installs the latest release in one step, and can also put the Windows installer in a Windows folder the Mac has mounted.

## How this fork stays current

GitHub Actions in this repository build and test both installers for every pull request, and:

- **Fork sync** (daily) merges each new Deskflow release, builds and tests it, and publishes it here. If a release doesn't merge cleanly, it opens an issue instead.
- **Fork canary** (weekly) tries Deskflow's unreleased changes and opens an issue if they would break this fork.

They use these repository secrets:

| Secret | What it's for |
|---|---|
| `FORK_CODESIGN_P12` (base64), `FORK_CODESIGN_P12_PWD`, `FORK_CODESIGN_ID` | Signing the Mac app with one certificate, so macOS keeps Deskflow's permissions across updates. Without them the app is signed ad hoc, and each update needs its permissions granted again. |
| `FORK_DEPLOY_KEY` | A deploy key with write access, for pushing upstream changes that touch workflow files. |

## License

The same as Deskflow: GPL-2.0 with the OpenSSL exception. See [LICENSE](https://github.com/AmrAbuelhamd/deskflow/blob/amr/main/LICENSE).
