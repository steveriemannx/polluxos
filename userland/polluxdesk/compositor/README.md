# PolluxOS Compositor

A small Wayland compositor for FreeBSD built on **wlroots 0.20** with a
macOS Big Sur / Sonoma look. The desktop UI (menu bar, dock) is drawn by the
dui `polluxdesk` client; the compositor itself owns outputs, input, the
wallpaper and every window's **server-side titlebar**.

## Architecture

```
wl_display (wayland-server)
└── wlr_backend (DRM / Wayland nested)
    └── wlr_scene graph
        ├── background tree        # compositor-drawn smooth 128-step Big Sur gradient
        ├── PolluxOS Desktop       # dui client, fullscreen, borderless, bottom
        └── app window tree        # one per xdg_toplevel
            ├── soft shadow pixmap # compositor-generated quadratic falloff
            ├── titlebar rect      # compositor-drawn (macOS/Windows style)
            │   ├── red close button
            │   ├── yellow minimize button
            │   └── green maximize button
            └── content tree       # client surface only
```

- xdg-decoration mode is forced to `SERVER_SIDE`: applications never draw their
  own titlebar, they only draw content.
- Clicking a titlebar drags the window; dragging the four edges or corners
  resizes it; traffic lights close / minimize (scratchpad) / maximize the
  window.
- The dui desktop shell is recognized by its Wayland title
  (`PolluxOS Desktop`) and is kept borderless behind every app window.
- `Alt+Tab` cycles windows and restores minimized ones.

## Build (FreeBSD)

The wlroots package on this host is `wlroots020-0.20.2`; its pkg-config file
has dependency-version metadata newer than the installed wayland 1.23. The
example's CMakeLists therefore locates `wlroots-0.20` manually and uses
pkg-config for `wayland-server`, `pixman-1` and `xkbcommon`.

```sh
cmake -S . -B build
cmake --build build --target polluxdesk-compositor
```

Output: `bin/polluxdesk-compositor`.

## Run / test

Nested inside an existing Wayland session:

```sh
WLR_BACKENDS=wayland WLR_RENDERER=pixman \
  ./bin/polluxdesk-compositor \
  -s 'foot & sleep 1; ./bin/polluxdesk'
```

Full sessions:

- login: `scripts/sessions/polluxdesk-compositor-greeter`
- desktop: `scripts/sessions/polluxdesk-compositor-desktop`

## License

MIT. Dependency stack: wlroots (MIT), wayland (MIT), pixman (MIT),
xkbcommon (MIT), dui (MIT), Skia (BSD). No GPL or LGPL components are used
by this compositor.
