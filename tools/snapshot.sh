#!/bin/sh
# Preserve a uniquely named snapshot. Never overwrite an earlier snapshot.
set -eu
cd "$(dirname "$0")/.."
LABEL="${1:-$(sed -n 's/^#define QP_VERSION_STR "\([^"]*\)"/\1/p' src/platform/version.h)}"
case "$LABEL" in
    ""|.|..|*[!A-Za-z0-9._-]*) echo "Invalid snapshot label" >&2; exit 1 ;;
esac
mkdir -p snapshots
snapshot_dir=$(mktemp -d "snapshots/$LABEL-XXXXXX")
git diff > "$snapshot_dir/tracked_changes.patch"
git status --short > "$snapshot_dir/git_status.txt"
for path in src third_party installer res tests tools docs; do
    [ ! -d "$path" ] || cp -R "$path" "$snapshot_dir/"
done
cp Makefile README.md .gitattributes .gitignore "$snapshot_dir/"
[ ! -f QuickPhrase.md ] || cp QuickPhrase.md "$snapshot_dir/"
mkdir -p "$snapshot_dir/dist"
for exe in dist/*.exe; do
    [ ! -f "$exe" ] || cp "$exe" "$snapshot_dir/dist/"
done
echo "Snapshot saved: $snapshot_dir"
