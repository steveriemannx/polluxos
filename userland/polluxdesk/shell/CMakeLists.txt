cmake_minimum_required(VERSION 3.18)

project(polluxdesk CXX)

# Per-executable source dir the dui helpers glob; the rest of the dui machinery
# (dependency setup, Skia) is already configured once by the top-level file.
get_filename_component(DUI_PROJECT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/" ABSOLUTE)
get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dui/" ABSOLUTE)

set(EMBED_RES_DIR "${DUI_ROOT}/resources")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_bin.cmake")
