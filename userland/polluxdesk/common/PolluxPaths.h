#pragma once

// Single source of truth for where polluxdesk's own binaries are installed.
//
// Adjacent string literal concatenation means POLLUX_BIN "/polluxdesk_files"
// is a compile-time constant, so it can be used inside the static const
// command tables that the dock and menu are built from.
//
// Keep this in step with the session scripts in ~/.local/bin if the build
// tree ever moves again: the shell, the launchpad and every menu entry that
// starts a PolluxOS app all resolve through here.
#define POLLUX_BIN "$HOME/projects-main/polluxos/build/polluxdesk/bin"
