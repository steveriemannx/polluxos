cmake_minimum_required(VERSION 3.18)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(polluxdesk CXX)

get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)
get_filename_component(DUI_PROJECT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/" ABSOLUTE)

# Mode 3 (pure code): the desktop shell UI is built entirely in C++ code
# (PolluxOSForm::OnInitWindow). A Wayland desktop shell styled after
# macOS Big Sur / Sonoma:
#   - Big Sur gradient wallpaper with translucent color blobs
#   - frosted top menu bar with native in-window dropdown menus + live clock
#   - centered frosted dock with colored app tiles that fork/exec apps
# Runs fullscreen inside the PolluxOS wlroots compositor.
include("${DUI_SRC_ROOT_DIR}/cmake/dui_common.cmake")

# Embed bin/resources into the executable as a custom binary archive (Qt qrc style, no zip)
set(EMBED_RES_DIR "${DUI_ROOT}/bin/resources")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")

include("${DUI_SRC_ROOT_DIR}/cmake/dui_bin.cmake")
