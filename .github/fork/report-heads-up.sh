#!/usr/bin/env bash
# Keeps a single "heads-up" issue for the weekly canary run.
#   report-heads-up.sh <report.md>         open the issue, or add the report as a comment
#   report-heads-up.sh --resolved <text>   close the issue with a final comment, if one is open
# Needs GH_TOKEN, GITHUB_REPOSITORY and ISSUE_TITLE in the environment.

set -euo pipefail

label="upstream-heads-up"
gh label create "$label" --repo "$GITHUB_REPOSITORY" --color FBCA04 \
  --description "Upstream master doesn't work with our changes yet" 2>/dev/null || true

existing=$(gh issue list --repo "$GITHUB_REPOSITORY" --state open --label "$label" \
  --search "\"$ISSUE_TITLE\" in:title" --json number -q '.[0].number')

if [ "${1:-}" == "--resolved" ]; then
  if [ -n "$existing" ]; then
    gh issue close "$existing" --repo "$GITHUB_REPOSITORY" --comment "$2"
  fi
  exit 0
fi

if [ -n "$existing" ]; then
  gh issue comment "$existing" --repo "$GITHUB_REPOSITORY" --body-file "$1"
else
  gh issue create --repo "$GITHUB_REPOSITORY" --title "$ISSUE_TITLE" --label "$label" --body-file "$1"
fi
