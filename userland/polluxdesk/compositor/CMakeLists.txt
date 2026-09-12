cmake_minimum_required(VERSION 3.18)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(polluxdesk_compositor C)

# ---------------------------------------------------------------------------
# PolluxOS compositor: wlroots 0.20 scene-graph compositor (MIT-licensed
# dependency stack only: wlroots / wayland / pixman / xkbcommon are MIT).
# The compositor draws macOS-style server-side titlebars with traffic lights;
# clients draw only their content surface.
#
# The installed wlroots-0.20 pkg-config file on this FreeBSD host has broken
# dependency version requirements (wayland 1.23 vs required 1.24), so we find
# the wlroots headers/library manually while pkg-config still supplies the
# transitive include/lib flags for wayland-server / pixman / xkbcommon.
# ---------------------------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(PIXMAN REQUIRED pixman-1)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
# Called directly to turn on tap-to-click on the touchpad: wlroots proxies
# pointers but configures no device, and the tap API is libinput's.
pkg_check_modules(LIBINPUT REQUIRED libinput)

find_path(WLROOTS_INCLUDE_DIR
    NAMES wlr/backend.h
    HINTS /usr/local/include
    PATH_SUFFIXES wlroots-0.20
    REQUIRED)
find_library(WLROOTS_LIBRARY
    NAMES wlroots-0.20
    HINTS /usr/local/lib
    REQUIRED)

add_executable(polluxdesk-compositor polluxdesk_compositor.c)

target_compile_definitions(polluxdesk-compositor PRIVATE
    WLR_USE_UNSTABLE
    _POSIX_C_SOURCE=200809L)

target_include_directories(polluxdesk-compositor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol"
    "${WLROOTS_INCLUDE_DIR}"
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS})

target_link_directories(polluxdesk-compositor PRIVATE /usr/local/lib)

# The FreeBSD host's wlroots package was built against a newer protocol mix;
# /usr/local/lib/libwlfixes.so provides the missing generated wl_fixes symbol.
target_link_libraries(polluxdesk-compositor PRIVATE
    "${WLROOTS_LIBRARY}"
    wlfixes
    ${WAYLAND_SERVER_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${LIBINPUT_LIBRARIES})

# Land next to the other desktop binaries when built from a tree that defines
# DUI_ROOT (the dui cmake modules); otherwise use this build tree. A bare
# ${DUI_ROOT}/bin would expand to /bin and the link would fail on permissions.
if(DEFINED DUI_ROOT AND IS_ABSOLUTE "${DUI_ROOT}")
    set_target_properties(polluxdesk-compositor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DUI_ROOT}/bin")
endif()
