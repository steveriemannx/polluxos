cmake_minimum_required(VERSION 3.20)
project(polluxdesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# dui 以 DUI_ROOT 指向 build/prefix（dui install 后的前缀树）。
# TODO: dui 若导出 CMake package(duiConfig.cmake)，改用 find_package(dui CONFIG)。
if(NOT DEFINED DUI_ROOT)
    set(DUI_ROOT "${CMAKE_INSTALL_PREFIX}")
endif()
find_path(DUI_INCLUDE_DIR dui.h PATH_SUFFIXES dui include HINTS "${DUI_ROOT}/include")
find_library(DUI_LIBRARY NAMES dui HINTS "${DUI_ROOT}/lib" "${DUI_ROOT}/lib64")
if(NOT DUI_INCLUDE_DIR OR NOT DUI_LIBRARY)
    message(FATAL_ERROR "dui not found under ${DUI_ROOT}, 先构建 dui (make dui)")
endif()

add_executable(polluxdesk
    main.cpp
)
target_include_directories(polluxdesk PRIVATE "${DUI_INCLUDE_DIR}")
target_link_libraries(polluxdesk PRIVATE "${DUI_LIBRARY}")

install(TARGETS polluxdesk DESTINATION bin)
# UI 资源(布局 XML、主题)铺到 /usr/local/share/polluxos
install(DIRECTORY ui/ DESTINATION share/polluxos)
