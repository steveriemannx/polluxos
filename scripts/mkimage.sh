#!/bin/sh
# 把 DESTDIR rootfs + 内核 + loader 组装成可 dd 的 UEFI 镜像:
#   polluxos.img (GPT)
#     p1: EFI FAT (EFI/BOOT/BOOTX64.EFI <- loader.efi)
#     p2: freebsd-ufs (rootfs, UFS2)
# 依赖: makefs / mkimg (FreeBSD base 自带; 15.x 起支持免 root 构建)。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-$ROOT/build}"
DESTDIR="$BUILD/root"
OBJPREFIX="$BUILD/obj"
PREFIX="$BUILD/prefix"
IMG="$BUILD/polluxos.img"
ESP_IMG="$BUILD/efiboot.img"
FS_IMG="$BUILD/rootfs.ufs"
ESP_SIZE="${ESP_SIZE:-260m}"

# 1. 用户态进 rootfs (dui/polluxdesk 装到 PREFIX，整体铺到 /usr/local)
mkdir -p "$DESTDIR/usr/local"
cp -R "$PREFIX"/. "$DESTDIR/usr/local/"

# 2. overlay/ 直铺 rootfs (rc.conf、登录脚本等)
#    注意: overlay 下先建 etc/usr/... 目录结构，保持与目标树一致
if [ -d overlay ]; then
    cp -R overlay/. "$DESTDIR/"
fi
cp config/rc.conf "$DESTDIR/etc/rc.conf"

# 3. 内核与 loader
#    loader.efi 出处在 MAKEOBJDIRPREFIX 下，路径不固定，用 find 找最近的
LOADER_EFI="$(find "$OBJPREFIX" -name loader.efi -path '*efi/loader/loader.efi' | head -1)"
[ -n "$LOADER_EFI" ] || { echo "error: loader.efi not found" >&2; exit 1; }
KERNDIR="$(find "$OBJPREFIX" -name kernel -type d -path '*amd64.*' | head -1)"
[ -n "$KERNDIR" ] || { echo "error: kernel dir not found" >&2; exit 1; }
cp "$KERNDIR"/kernel "$DESTDIR/boot/kernel/kernel"
cp "$KERNDIR"/*.ko "$DESTDIR/boot/kernel/" 2>/dev/null || true

# 4. ESP(FAT) 与 rootfs(UFS2) 两个分区镜像
rm -f "$ESP_IMG" "$FS_IMG" "$IMG"
ESP_DIR="$BUILD/esp"
rm -rf "$ESP_DIR"; mkdir -p "$ESP_DIR/EFI/BOOT"
cp "$LOADER_EFI" "$ESP_DIR/EFI/BOOT/BOOTX64.EFI"
makefs -t msdos -s "$ESP_SIZE" -o volume_label=POLLUXOS "$ESP_IMG" "$ESP_DIR"

makefs -t ufs -o version=2 "$FS_IMG" "$DESTDIR"

# 5. GPT 拼装
mkimg -s gpt \
    -p efi:="$ESP_IMG" \
    -p freebsd-ufs:="$FS_IMG" \
    -o "$IMG"

echo "image: $IMG"
ls -lh "$IMG"
