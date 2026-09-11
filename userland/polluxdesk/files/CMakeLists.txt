cmake_minimum_required(VERSION 3.18)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(polluxdesk_files CXX)

get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)
get_filename_component(DUI_PROJECT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/" ABSOLUTE)

# Mode 3 (pure code): the UI is built entirely in C++ code. A Finder-style
# file manager window for PolluxOS (macOS Big Sur light style). Runs as a
# regular floating app window inside the PolluxOS wlroots compositor on
# FreeBSD (the compositor draws the titlebar / traffic lights / shadow).
include("${DUI_SRC_ROOT_DIR}/cmake/dui_common.cmake")

# Embed bin/resources into the executable as a custom binary archive (Qt qrc style, no zip)
set(EMBED_RES_DIR "${DUI_ROOT}/bin/resources")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")

include("${DUI_SRC_ROOT_DIR}/cmake/dui_bin.cmake")
