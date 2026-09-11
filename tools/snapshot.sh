#!/bin/sh
# 把当前工作完整快照到 snapshots/<标签>/（snapshots/ 已被 .gitignore 忽略，不污染仓库状态）。
# 用途：无论之后回退到哪一版，这一版都还能找回来。
#
# 放在 build/ 之外是有意的 —— build/ 会被 `make clean` 整个删掉，快照放那儿会被误删。
#
#   tools/snapshot.sh            标签默认取 Makefile 里的 VERSION
#   tools/snapshot.sh 3.2.0      手动指定标签
set -e
cd "$(dirname "$0")/.."

LABEL="${1:-$(sed -n 's/^VERSION   := //p' Makefile)}"
[ -n "$LABEL" ] || LABEL=unknown
B="snapshots/$LABEL"

rm -rf "$B"
mkdir -p "$B/tests" "$B/dist"

# 被跟踪文件的改动（未提交的那部分）
git diff > "$B/tracked_changes.patch"
git status --short > "$B/git_status.txt"

# 未跟踪的新文件（git diff 里没有它们）
for f in src/*.c src/*.h; do
    case "$(git ls-files --error-unmatch "$f" 2>/dev/null)" in
        "$f") ;;
        *) [ -f "$f" ] && cp "$f" "$B/" ;;
    esac
done

# 本机保留、不进版本库的东西
cp QuickPhrase.md "$B/" 2>/dev/null || true
cp tests/*.c tests/*.py "$B/tests/" 2>/dev/null || true
cp dist/*.exe "$B/dist/" 2>/dev/null || true
cp build/QuickPhrase.exe "$B/dist/QuickPhrase.prev-build.exe" 2>/dev/null || true
cp Makefile "$B/"

echo "已快照到 $B ："
ls -lR "$B" | head -40
