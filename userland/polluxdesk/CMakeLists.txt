cmake_minimum_required(VERSION 3.18)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# PolluxOS desktop: ONE cmake tree so dui's dependency machinery runs once.
# Splitting the components into separate projects made each one build its own
# copy of Skia (dui's DUI_DEPS_CONFIGURED guard is a CMake GLOBAL property, which
# only spans a single tree).
project(polluxdesk CXX)

# Skia ships C sources (expat, libpng, ...); without C they get compiled by the
# C++ driver and fail on implicit void* conversions.
enable_language(C)

get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../dui/" ABSOLUTE)

# Reuse the Skia already built by the dui build tree instead of rebuilding it
# here. dui's own cmake skips regeneration when libskia.a is already present.
set(_pd_dui_build "${CMAKE_CURRENT_LIST_DIR}/../../build/dui")
set(DUI_BUILD_SKIA_FROM_SOURCE OFF CACHE BOOL "" FORCE)
set(DUI_SKIA_LIB_PATH "${CMAKE_BINARY_DIR}/lib/release" CACHE PATH "" FORCE)
set(DUI_SKIA_LIB_PATH_RELEASE "${CMAKE_BINARY_DIR}/lib/release" CACHE PATH "" FORCE)

include("${DUI_SRC_ROOT_DIR}/cmake/dui_common.cmake")

if(NOT EXISTS "${DUI_SKIA_LIB_PATH_RELEASE}/libskia.a")
    file(MAKE_DIRECTORY "${DUI_SKIA_LIB_PATH_RELEASE}")
    file(GLOB _pd_skia "${_pd_dui_build}/lib/release/*")
    foreach(_f IN LISTS _pd_skia)
        get_filename_component(_n "${_f}" NAME)
        execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${_f}" "${DUI_SKIA_LIB_PATH_RELEASE}/${_n}")
    endforeach()
endif()

# `dui` itself is already built in the dui tree; this tree builds only the
# desktop executables that link against it.
link_directories("${_pd_dui_build}/lib" "${_pd_dui_build}/lib/release")
if(NOT DEFINED EPOLL_SHIM_LIBRARY AND EXISTS "/usr/local/lib/libepoll-shim.so")
    set(EPOLL_SHIM_LIBRARY "/usr/local/lib/libepoll-shim.so")
endif()

foreach(component shell apps files settings)
    add_subdirectory(${component})
endforeach()
