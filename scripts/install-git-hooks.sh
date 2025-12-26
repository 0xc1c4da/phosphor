#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "install-git-hooks: not a git repo (no .git directory found)" >&2
  exit 2
fi

src="${repo_root}/scripts/git-hooks/pre-push"
dst="${repo_root}/.git/hooks/pre-push"

mkdir -p "${repo_root}/.git/hooks"
cp -f "${src}" "${dst}"
chmod +x "${dst}"

echo "install-git-hooks: installed pre-push hook -> ${dst}"
echo "install-git-hooks: skip with PHOS_SKIP_I18N_VALIDATE=1 git push"


