# PolluxOS 顶层总控 —— 注意: 各子系统的构建系统保持官方原样:
#   FreeBSD base: bmake  base/freebsd-src/
#   dui/polluxdesk: CMake  userland/
# 本 Makefile 只做调度与编排，且只在 FreeBSD 上跑（world/kernel 也必须在 FreeBSD 上），
# 所以用系统自带的 bmake，不需要 gmake。dui/polluxdesk 那层由 CMake 驱动。

ROOT      ?= ${.CURDIR}
BUILD     ?= $(ROOT)/build
BASE      ?= $(ROOT)/base/freebsd-src
KERNCONF  ?= POLLUXOS
DESTDIR   ?= $(BUILD)/root
OBJPREFIX ?= $(BUILD)/obj
PREFIX    ?= $(BUILD)/prefix
IMG       ?= $(BUILD)/polluxos.img
JOBS      != sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8
MWORLDS   != expr $(JOBS) \* 2

.PHONY: all fetch world kernel dui userland image qemu clean

all: image

# ---- 子模块/补丁 -------------------------------------------------------
fetch:
	sh scripts/fetch.sh

# ---- FreeBSD base (bmake) ----------------------------------------------
# KERNCONF 通过拷贝注入(不动 base 文件)，随后打补丁
world: fetch
	cp config/KERNCONF/$(KERNCONF) $(BASE)/sys/amd64/conf/$(KERNCONF)
	sh scripts/apply-patches.sh
	env MAKEOBJDIRPREFIX=$(OBJPREFIX) SRCCONF=$(ROOT)/config/src.conf \
	    make -C $(BASE) buildworld -j$(MWORLDS)

kernel: world
	env MAKEOBJDIRPREFIX=$(OBJPREFIX) SRCCONF=$(ROOT)/config/src.conf \
	    make -C $(BASE) buildkernel KERNCONF=$(KERNCONF) -j$(JOBS)

# ---- dui / polluxdesk (CMake) -------------------------------------------
# dui needs CMake >= 4.0 and FreeBSD ports/package tree stops at 3.31, so prefer
# a locally built cmake installed as `cmake4`.  Override with e.g.
# make CMAKE=/path/to/cmake.
#
# CMake drives its own build files with the make below.  bmake is the same name on
# FreeBSD (/usr/bin/bmake, the same binary as make) and on macOS (Homebrew), so no
# gmake is needed; override it for another backend, e.g. make CMAKE_MAKE_PROGRAM=gmake
CMAKE != command -v cmake4 2>/dev/null || echo cmake

# 生成器。默认 Unix Makefiles 配上 bmake（FreeBSD 与 macOS 同名，无需 gmake）。
# 换生成器只在构建目录首次创建时生效，例如 make GENERATOR=Ninja dui
# （此后 build/dui 必须重新生成，CMake 不允许就地改生成器）。
GENERATOR ?= Unix Makefiles

# Ninja 自带构建程序，且与 CMAKE_MAKE_PROGRAM 同时给出会被 CMake 拒绝。
.if !empty(GENERATOR:M*Makefiles)
CMAKE_MAKE_PROGRAM ?= bmake
BACKEND = -DCMAKE_MAKE_PROGRAM=$(CMAKE_MAKE_PROGRAM)
.else
BACKEND =
.endif

dui:
	$(CMAKE) -S userland/dui -G "$(GENERATOR)" -B $(BUILD)/dui -DCMAKE_BUILD_TYPE=Release \
	    -DDUI_ENABLE_WAYLAND=ON -DDUI_ENABLE_SDL=OFF \
	    -DDUI_BUILD_EXAMPLES=OFF $(BACKEND)
	$(CMAKE) --build $(BUILD)/dui -j$(JOBS)
	$(CMAKE) --install $(BUILD)/dui --prefix $(PREFIX)

userland: dui
	$(CMAKE) -S userland/polluxdesk -G "$(GENERATOR)" -B $(BUILD)/polluxdesk -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_PREFIX_PATH=$(PREFIX) -DDUI_ROOT=$(PREFIX) \
	    -DDUI_ENABLE_WAYLAND=ON $(BACKEND)
	$(CMAKE) --build $(BUILD)/polluxdesk -j$(JOBS)
	$(CMAKE) --install $(BUILD)/polluxdesk --prefix $(PREFIX)

# ---- 镜像 ---------------------------------------------------------------
image: kernel userland
	sh scripts/mkimage.sh

qemu: image
	sh scripts/run-qemu.sh

clean:
	rm -rf $(BUILD)
