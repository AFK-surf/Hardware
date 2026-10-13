#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copy the public project into DEST (internal tool, not exported).
#
# Usage: tools/export_public.sh DEST
#
# Only files tracked by Git at HEAD are copied, so untracked and ignored files (secrets.h, generated
# sdkconfig, build-*, managed_components, test certificates, local caches) never leave. Uncommitted
# edits to tracked files are not exported either: commit first. DEST must not exist or be empty.
# Then run: tools/check_public.py DEST
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST=${1:?Usage: tools/export_public.sh DEST}

# Internal paths (Git pathspecs), each with the reason it stays out.
EXCLUDE=(
  'docs/prompts/'                   # agent instructions for each development step
  'docs/reports/'                   # development reports: test networks, device IDs, logs
  'docs/connector-plan.md'          # internal planning notes: backend file paths, test boards and networks
  'firmware/reference/comma-logo/'  # logo animation source copied from the private app repository
  'tools/mini_connector.py'         # protocol spike against a local backend; fake_connector.py covers testing
  'tools/check_public.py'           # this export's leak check: holds the private deny list
  'tools/export_public.sh'          # this script
)

if [ -e "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then
  echo "DEST must not exist or be empty: $DEST" >&2
  exit 2
fi
mkdir -p "$DEST"

cd "$ROOT"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "warning: uncommitted changes to tracked files are not exported (HEAD is)" >&2
fi
PATHSPEC=(.)
for path in "${EXCLUDE[@]}"; do PATHSPEC+=(":(exclude)$path"); done
# git archive writes HEAD's tracked files only; export-ignore attributes do not apply here.
git archive --format=tar HEAD -- "${PATHSPEC[@]}" | tar -x -C "$DEST"
echo "Exported $(find "$DEST" -type f | wc -l | tr -d ' ') files to $DEST"
