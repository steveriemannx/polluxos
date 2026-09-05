#!/bin/sh
# 把 patches/*.patch 依序应用到 base/freebsd-src。
# 补丁要求: 由 git format-patch 生成(含文件头上下文)，且路径相对 base 仓库根。
# 已应用的补丁会自动跳过(git apply 反查失败即认为已应用)。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$ROOT/base/freebsd-src"

cd "$BASE"
if ! git diff --quiet; then
    echo "error: base/freebsd-src 有未提交改动，先清理再打补丁" >&2
    exit 1
fi

for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    if git apply --reverse --check "$p" 2>/dev/null; then
        echo "skip  $(basename "$p")  (already applied)"
    else
        echo "apply $(basename "$p")"
        git apply "$p"
    fi
done
echo "patches applied"
