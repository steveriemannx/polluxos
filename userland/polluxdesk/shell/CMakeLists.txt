cmake_minimum_required(VERSION 3.18)

project(polluxdesk CXX)

# Per-executable source dir the dui helpers glob; the rest of the dui machinery
# (dependency setup, Skia) is already configured once by the top-level file.
get_filename_component(DUI_PROJECT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/" ABSOLUTE)
get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dui/" ABSOLUTE)

set(EMBED_RES_DIR "${DUI_ROOT}/resources")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")
# PolluxOS headers shared by the components (PolluxPaths.h, ...).
include_directories("${CMAKE_CURRENT_LIST_DIR}/../common")
# The Wi-Fi data layer lives with the Wi-Fi window; the shell's popover uses
# it too, so the panel and the window can never disagree about the networks.
include_directories("${CMAKE_CURRENT_LIST_DIR}/../wifi")

include("${DUI_SRC_ROOT_DIR}/cmake/dui_bin.cmake")

# Joined into the shell target after dui_bin.cmake created it -- the glob in
# there would drop anything added to SRC_FILES before the include.
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../wifi/WifiData.cpp")
