#!/bin/sh
# PolluxOS 构建主机准备 —— 在 FreeBSD 15.x 上以 root 运行。
# 安装 base 构建 + dui(Wayland 后端) 所需的依赖。
# 若是刚装完系统,先: pkg update
set -e

PKGS="git cmake ninja pkgconf python3 wayland wayland-protocols mesa-libs"

case "$(uname -s)" in
    FreeBSD) ;;
    *) echo "error: 在 FreeBSD 上运行 (当前: $(uname -s))" >&2; exit 1 ;;
esac

pkg install -y $PKGS

echo
echo "dui 依赖检查(可选,若有 pkg-config 报错再补):"
echo "  pkg-config --exists wayland-client wayland-egl wayland-cursor && echo ok"
echo
echo "构建主机准备完成。下一步: clone polluxos + make"
