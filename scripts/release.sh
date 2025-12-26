#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

die() { echo "release: $*" >&2; exit 1; }
note() { echo "release: $*"; }

base="$(tr -d '\r\n' < VERSION 2>/dev/null || true)"
[ -n "$base" ] || die "VERSION file is empty/missing"

# Strict SemVer (no prerelease/build metadata) for tags; you can extend later if desired.
if ! [[ "$base" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  die "VERSION must be SemVer 'X.Y.Z' (got: '$base')"
fi

tag="v$base"

# Require a clean tree for a release.
if ! git diff --quiet || ! git diff --cached --quiet; then
  die "working tree is dirty (commit or stash changes before tagging a release)"
fi

# Do not allow tagging a commit already tagged with a different version.
head_tag="$(git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)"
if [ -n "$head_tag" ] && [ "$head_tag" != "$tag" ]; then
  die "HEAD is already tagged as '$head_tag' (refusing to also tag '$tag')"
fi

# Tag must not already exist elsewhere.
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
  if [ -n "$head_tag" ] && [ "$head_tag" = "$tag" ]; then
    note "tag '$tag' already exists on HEAD (nothing to do)"
    exit 0
  fi
  die "tag '$tag' already exists (bump VERSION first)"
fi

# Ensure this is a version bump compared to the latest existing vX.Y.Z tag (if any).
latest="$(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | sort -V | tail -n 1 || true)"
if [ -n "$latest" ]; then
  python3 - "$latest" "$tag" <<'PY'
import re, sys

def parse(tag: str):
    m = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", tag.strip())
    if not m:
        raise SystemExit(2)
    return tuple(map(int, m.groups()))

latest, new = sys.argv[1], sys.argv[2]
la, ne = parse(latest), parse(new)
if ne <= la:
    print(f"release: VERSION ({new[1:]}) is not a bump over latest tag ({latest[1:]})", file=sys.stderr)
    sys.exit(1)
PY
fi

if [ "${DRY_RUN:-0}" = "1" ]; then
  note "DRY_RUN=1; would run: git tag -a '$tag' -m 'Phosphor $tag'"
  exit 0
fi

note "tagging HEAD with '$tag'"
git tag -a "$tag" -m "Phosphor $tag"

note "created tag '$tag'"
note "next: git push origin '$tag'  (or: git push --tags)"


