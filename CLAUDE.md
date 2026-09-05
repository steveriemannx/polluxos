# PolluxOS

## What is this

PolluxOS is a desktop operating system built on **FreeBSD 15.0-STABLE** with **dui**
(a C++ GUI library: XML-described layout + Skia rendering) as its GUI framework.
dui talks to the display stack directly via its **native Wayland backend**
(`-DDUI_ENABLE_WAYLAND=ON -DDUI_ENABLE_SDL=OFF`); SDL is deliberately not used.

Boot flow: kernel (vt/efifb or DRM) → init → Wayland compositor (weston/sway) →
**stardesk**, the dui-based desktop shell (taskbar/launcher, XML layouts under
`userland/stardesk/ui/`).

The build produces a **dd-able UEFI image** (`build/polluxos.img`, GPT:
FAT EFI partition with loader.efi + UFS2 rootfs). No installer yet.

## Build system split (this is intentional — do not "unify" it)

| Component | Build system | Notes |
| :--- | :--- | :--- |
| `base/freebsd-src/` (submodule) | **bmake** | `make buildworld/buildkernel`, KERNCONF, src.conf |
| `userland/dui/` (submodule, URL TBD) | **CMake** | Wayland backend enabled, SDL disabled |
| `userland/stardesk/` | **CMake** | Links against installed dui under `build/prefix` |
| top-level `Makefile` | plain make | Orchestration only; never reimplements the above |

## Key commands

```sh
make               # full chain: world → kernel → dui → stardesk → polluxos.img
make world         # buildworld (+ injects KERNCONF, applies patches)
make kernel        # buildkernel KERNCONF=POLLUXOS
make dui           # cmake configure/build/install dui into build/prefix
make image         # assemble dd-able build/polluxos.img
make qemu          # boot the image in QEMU
sh scripts/fetch.sh && sh scripts/apply-patches.sh
```

**Constraint:** world/kernel builds must run **on FreeBSD** (official tooling
does not support cross-building base from macOS/Linux). Code edits can be made
on macOS; the default build host is a **physical SSD machine** running
FreeBSD 15.x (setup: `scripts/setup-build-host.sh`); a QEMU VM on macOS is the
fallback if the host disk allows (~40 GB).

## Hard rules

1. **Never edit `base/freebsd-src/` directly.** All base modifications must be
   `git format-patch` output in `patches/NNNN-desc.patch`, applied by
   `scripts/apply-patches.sh` (idempotent; skips already-applied patches).
   The kernel config is injected by copying `config/KERNCONF/$KERNCONF` into
   `base/sys/amd64/conf/` at build time — the submodule stays clean.
2. **Duo submodule updates:** bump the pinned commit in `base/freebsd-src` by
   fetching stable/15 (`git fetch origin stable/15 --depth 1`), or pin a fixed
   SHA in `config/base.sha`. Never merge upstream into a local branch in-tree.
3. **FreeBSD base ≠ X11/Wayland.** src.conf only controls base components
   (games/man/tests/...); the X11/Wayland userland stack is entirely
   ports/pkg-level (see `config/make.conf`).
4. **Commits:** all commits in this repo must end with the trailer
   `Co-Authored-By: Claude <noreply@anthropic.com>`.

## Layout

```
base/freebsd-src/   FreeBSD stable/15 (submodule, bmake)
config/             KERNCONF/POLLUXOS, src.conf, make.conf, rc.conf (build/runtime config)
patches/            base patches (git format-patch) + README
userland/dui/       dui submodule (URL TBD — not added yet; fetch.sh tolerates its absence)
userland/stardesk/  desktop shell: CMakeLists.txt, main.cpp, ui/shell.xml
overlay/            files copied verbatim into the rootfs
scripts/            fetch.sh, apply-patches.sh, build.sh, mkimage.sh, run-qemu.sh
```

## Current status / TODO

- dui submodule: **pending** — user must supply the repo URL, then
  `git submodule add <url> userland/dui`
- stardesk: placeholder main.cpp; real taskbar/launcher UI not written yet
- compositor choice (weston vs sway) undecided; `config/rc.conf` has commented templates
- installer: none; dd-able image only (image-side only; plan: bsdinstall-based later)
