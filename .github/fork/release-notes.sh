#!/usr/bin/env bash
# Prints the notes for a fork release: release-notes.sh <upstream tag> <commit>
# Used by fork-release.yml, and by hand to rewrite an existing release's notes.

set -euo pipefail

base="$1"
commit="$2"
repo="${GITHUB_REPOSITORY:-AmrAbuelhamd/deskflow}"

echo "Deskflow [${base#v}](https://github.com/deskflow/deskflow/releases/tag/${base}) as built by this fork. The [README](https://github.com/${repo}#readme) says what the fork adds."
echo
echo "### Install"
echo "- Mac with Apple silicon: \`deskflow-*-macos-arm64.dmg\`. The first time, if macOS can't check it for malware, click **Open Anyway** in System Settings → Privacy & Security."
echo "- Windows (x64): \`deskflow-*-win-x64.msi\`, or the portable \`.zip\`. If SmartScreen stops it, click **More info**, then **Run anyway**."
echo
echo "SHA-256 checksums are in \`sums.txt\`."
echo
echo "### Changes on top of Deskflow ${base#v}"
git log --no-merges --format='- %s' "${base}..${commit}"
