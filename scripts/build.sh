#!/bin/sh
# PolluxOS 全量构建(等于 make all 的手动版):
#   buildworld -> buildkernel -> dui -> installworld/installkernel 到 DESTDIR
#   -> 组装 rootfs -> mkimg
# 注意: 必须在 FreeBSD 上运行(官方不支持从 macOS 交叉构建 base)。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-$ROOT/build}"
BASE="$ROOT/base/freebsd-src"
KERNCONF="${KERNCONF:-POLLUXOS}"
DESTDIR="$BUILD/root"
PREFIX="$BUILD/prefix"
OBJPREFIX="$BUILD/obj"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
MWORLDS="${MWORLDS:-$((JOBS * 2))}"

# 1. 子模块 & 补丁
sh scripts/fetch.sh
mkdir -p "$BUILD"

# KERNCONF 由拷贝注入，不改 base 文件(打补丁除外)
cp config/KERNCONF/$KERNCONF "$BASE/sys/amd64/conf/$KERNCONF"
sh scripts/apply-patches.sh

# 2. FreeBSD base (bmake)
env MAKEOBJDIRPREFIX="$OBJPREFIX" \
    SRCCONF="$ROOT/config/src.conf" \
    make -C "$BASE" buildworld -j "$MWORLDS" -DNO_CLEAN

env MAKEOBJDIRPREFIX="$OBJPREFIX" \
    SRCCONF="$ROOT/config/src.conf" \
    make -C "$BASE" buildkernel KERNCONF="$KERNCONF" -j "$JOBS"

# installworld/installkernel -> DESTDIR
env MAKEOBJDIRPREFIX="$OBJPREFIX" \
    make -C "$BASE" installworld DESTDIR="$DESTDIR"
env MAKEOBJDIRPREFIX="$OBJPREFIX" \
    make -C "$BASE" installkernel DESTDIR="$DESTDIR" KERNCONF="$KERNCONF"

# 3. dui + polluxdesk (cmake)，安装到统一 PREFIX/前缀树
cmake -S userland/dui -B "$BUILD/dui" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDUI_ENABLE_WAYLAND=ON -DDUI_ENABLE_SDL=OFF
cmake --build "$BUILD/dui" -j "$JOBS"
cmake --install "$BUILD/dui" --prefix "$PREFIX"

cmake -S userland/polluxdesk -B "$BUILD/polluxdesk" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" -DDUI_ROOT="$PREFIX"
cmake --build "$BUILD/polluxdesk" -j "$JOBS"
cmake --install "$BUILD/polluxdesk" --prefix "$PREFIX"

# 4. 根目录组装 + 镜像
sh scripts/mkimage.sh

echo
echo "OK: $BUILD/polluxos.img 生成。"
echo "  dd 到 U 盘:  dd if=$BUILD/polluxos.img of=/dev/diskX bs=1m conv=sync"
echo "  QEMU 运行:  make qemu"
