cmake_minimum_required(VERSION 3.18)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(polluxdesk_apps CXX)

get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)
get_filename_component(DUI_PROJECT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/" ABSOLUTE)

include("${DUI_SRC_ROOT_DIR}/cmake/dui_common.cmake")
set(EMBED_RES_DIR "${DUI_ROOT}/bin/resources")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_bin.cmake")
