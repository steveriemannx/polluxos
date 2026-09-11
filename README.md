# PolluxOS

PolluxOS is a new operating system built on **FreeBSD 15.0-STABLE** with **dui**
(a cross-platform C++ UI library using XML-described layout + Skia rendering)
as its GUI framework. dui connects to the display stack through its native
**Wayland backend** (`-DDUI_ENABLE_WAYLAND=ON -DDUI_ENABLE_SDL=OFF`) — SDL is
not used.

The goal is a dd-able UEFI image: write `build/polluxos.img` to a USB stick or
disk and boot straight into the polluxdesk desktop shell (taskbar, launcher,
dui + Skia rendering).

## Layout

| Path | Purpose | Build system |
| :--- | :--- | :--- |
| `base/freebsd-src/` | FreeBSD source (official GitHub mirror, submodule, stable/15) | bmake |
| `config/` | KERNCONF / src.conf / make.conf / rc.conf (build & runtime config) | — |
| `patches/` | Patches against base (git format-patch) | — |
| `userland/dui/` | dui library (submodule, Wayland backend) | CMake |
| `userland/polluxdesk/` | Desktop shell (taskbar/launcher, drawn by dui) | CMake |
| `overlay/` | Files copied verbatim into the rootfs | — |
| `scripts/` | fetch / apply-patches / build / mkimage / run-qemu / setup-build-host | — |

## Development loop

**Prerequisite:** `make buildworld` can only run **on FreeBSD** — base does not
support cross-building from macOS/Linux. Two setups:

**A. Physical SSD as the build host (recommended — keeps big build artifacts off
your Mac).**

1. On macOS, download the FreeBSD 15.1 memstick image and dd it to a USB stick:
   ```sh
   curl -O https://download.freebsd.org/releases/amd64/amd64/15.1-RELEASE/memstick.iso
   dd if=memstick.iso of=/dev/diskX bs=1m conv=sync
   ```
2. Boot the USB stick and install FreeBSD (UFS2 is fine) onto the SSD.
3. On the SSD machine, run `sh scripts/setup-build-host.sh` (installs
   git/cmake/ninja/pkgconf/python3 + the Wayland build deps dui needs).
4. `git clone` polluxos and init the base submodule:
   ```sh
   git clone <polluxos-url> && cd polluxos
   git submodule update --init --depth 1 base/freebsd-src
   ```
5. `make world && make kernel && make dui && make userland && make image`
6. Test on real hardware: `dd if=build/polluxos.img of=/dev/diskY bs=1m conv=sync`,
   then boot that device.

**B. QEMU VM on macOS** — needs roughly 40 GB of qcow2 (world obj ~20 GB +
DESTDIR ~6 GB + dui/Skia ~3 GB + image). Only when your host disk allows it.

```sh
# First time: fetch freebsd-src (shallow, ~1 GB). dui submodule will be added
# once its repo URL is decided:
#   git submodule add <dui repo url> userland/dui
git submodule update --init --depth 1 base/freebsd-src

# Full chain: buildworld -> buildkernel -> dui/polluxdesk -> polluxos.img
make -j8

# Or step by step
make world && make kernel && make dui && make userland
make image

# Boot the image in a VM
make qemu

# Write to disk / USB (mind the device name!)
dd if=build/polluxos.img of=/dev/diskX bs=1m conv=sync
```

## Display stack

```
kernel (vt + efifb/drm) ──► /dev/dri KMS ──► mesa/EGL ──► dui (PolluxOS drawing)
                                   │
          weston / sway (compositor) ┘
            └─ polluxdesk runs as a Wayland client
```

- The FreeBSD kernel provides KMS/DRM and vt(4); the userland Wayland stack
  comes from ports/pkg; dui builds with `-DDUI_ENABLE_WAYLAND=ON
  -DDUI_ENABLE_SDL=OFF`.
- The image is GPT (FAT EFI partition with loader.efi + UFS2 rootfs): dd it and
  boot. No installer yet — a minimal "pick a disk, gpart + dd" installer can be
  written later if needed.

## Updating FreeBSD upstream

```sh
cd base/freebsd-src
git fetch origin stable/15 --depth 1
git checkout FETCH_HEAD          # or pin a fixed commit via config/base.sha
cd ../..
make world                        # apply-patches replays patches (conflicts visible at once)
```

---

## Note on the `Co-Authored-By: Claude` trailer

Commits in this repository ending with a `Co-Authored-By: Claude
<noreply@anthropic.com>` trailer simply indicate that AI-assisted tooling was
used in producing that commit. The trailer is **not** an attribution to any
specific model — the assistance may involve third-party models and tools at
various points. The only specific tool it refers to is [Claude Code]
(https://claude.com/claude-code), the environment in which the work was done.
