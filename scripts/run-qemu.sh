#!/bin/sh
# 直接启动构建出的 polluxos.img，验证开发。
# 在 macOS 上 qemu 可有 cocoa 显示; FreeBSD 上默认 gtk。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${IMG:-$ROOT/build/polluxos.img}"

[ -f "$IMG" ] || { echo "error: $IMG 不存在,先 make image" >&2; exit 1; }

M="${M:-4096}"
SMP="${SMP:-4}"

exec qemu-system-x86_64 \
    -m "$M" -smp "$SMP" \
    -drive file="$IMG",format=raw,if=virtio \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -serial mon:stdio \
    "$@"
