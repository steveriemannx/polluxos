#!/bin/sh
# 初始化/更新子模块。base/freebsd-src 大(浅克隆~1GB)，--depth 1 只取当前 commit。
# 若要 pin 到指定 commit: config/base.sha (40位 sha)，fetch 后 checkout。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

git submodule update --init --depth 1 base/freebsd-src

# dui 子模块(尚未添加时跳过；添加后自动纳入)
if git config --file .gitmodules --get submodule.userland/dui.url >/dev/null 2>&1; then
    git submodule update --init --depth 1 userland/dui
fi

BASE="$ROOT/base/freebsd-src"
if [ -f config/base.sha ]; then
    SHA="$(cat config/base.sha)"
    git -C "$BASE" fetch --depth 1 origin "$SHA"
    git -C "$BASE" checkout "$SHA"
    echo "base pinned to $SHA (config/base.sha)"
fi

echo "fetch done:"
echo "  base:  $(git -C "$BASE" rev-parse --short HEAD) $(git -C "$BASE" describe --always 2>/dev/null || true)"
echo "  dui:   $(git -C "$ROOT/userland/dui" rev-parse --short HEAD)"
