#!/usr/bin/env bash
# Installs a release of this fork on this Mac and puts the Windows installer on the
# Windows share, ready to double-click.
#
#   deskflow-update.sh            install the latest release
#   deskflow-update.sh <tag>      install a specific release, e.g. v1.27.0-amr.1
#   deskflow-update.sh --rollback put back the app that the last update replaced
#
# Environment overrides: DESKFLOW_REPO, DESKFLOW_WINDOWS_DOWNLOADS

set -euo pipefail

repo="${DESKFLOW_REPO:-AmrAbuelhamd/deskflow}"
windows_downloads="${DESKFLOW_WINDOWS_DOWNLOADS:-/Volumes/amr98/Downloads}"
app="/Applications/Deskflow.app"
keep="$HOME/Library/Caches/deskflow-update"
previous="$keep/Deskflow-previous.app"

deskflow_running() {
  pgrep -xq Deskflow || pgrep -xq deskflow-core
}

wait_for_exit() {
  for _ in $(seq 1 20); do
    deskflow_running || return 0
    sleep 0.5
  done
  return 1
}

quit_deskflow() {
  deskflow_running || return 0
  echo "Quitting Deskflow..."
  osascript -e 'quit app "Deskflow"' > /dev/null 2>&1 || true
  wait_for_exit && return 0
  # Deskflow can refuse that (error -128) and stay in the menu bar. It quits on
  # SIGTERM as it does from its menu, stopping deskflow-core with it.
  pkill -TERM -x Deskflow || true
  wait_for_exit && return 0
  pkill -TERM -x deskflow-core || true
  wait_for_exit && return 0
  echo "Deskflow is still running. Quit it from its menu bar icon, then run this again." >&2
  exit 1
}

if [ "${1:-}" == "--rollback" ]; then
  [ -d "$previous" ] || { echo "No previous version to roll back to." >&2; exit 1; }
  quit_deskflow
  rm -rf "$app"
  ditto "$previous" "$app"
  open "$app"
  echo "Rolled back to: $(defaults read "$app/Contents/Info.plist" CFBundleShortVersionString)"
  exit 0
fi

command -v gh > /dev/null || { echo "The GitHub CLI (gh) is required." >&2; exit 1; }

tag="${1:-$(gh release view --repo "$repo" --json tagName -q .tagName)}"
echo "Installing $tag from $repo"

work=$(mktemp -d)
mnt="$work/mnt"
cleanup() {
  hdiutil detach "$mnt" -quiet 2> /dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

gh release download "$tag" --repo "$repo" --dir "$work" \
  --pattern '*-macos-arm64.dmg' --pattern '*-win-x64.msi' --pattern 'sums.txt'
(cd "$work" && shasum -a 256 --ignore-missing -c sums.txt)

dmg=$(ls "$work"/*-macos-arm64.dmg)
msi=$(ls "$work"/*-win-x64.msi)

# The disk image shows a license agreement; "Y" accepts it.
mkdir "$mnt"
echo "Y" | hdiutil attach "$dmg" -nobrowse -noautoopen -mountpoint "$mnt" > /dev/null
codesign --verify --deep --strict "$mnt/Deskflow.app"

quit_deskflow
mkdir -p "$keep"
if [ -d "$app" ]; then
  rm -rf "$previous"
  mv "$app" "$previous"
fi
ditto "$mnt/Deskflow.app" "$app"
echo "Installed: $(defaults read "$app/Contents/Info.plist" CFBundleShortVersionString)"
open "$app"

if [ -d "$windows_downloads" ]; then
  cp "$msi" "$windows_downloads/"
  echo "Windows installer copied to $windows_downloads/$(basename "$msi")"
  echo "On Windows, double-click it in Downloads."
else
  cp "$msi" "$HOME/Downloads/"
  echo "The Windows share isn't mounted; the installer is in ~/Downloads/$(basename "$msi")"
fi
