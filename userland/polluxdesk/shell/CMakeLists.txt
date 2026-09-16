cmake_minimum_required(VERSION 3.18)

project(polluxdesk CXX)

# The rest of the dui machinery (dependency setup, Skia) is already configured
# once by the top-level file.
get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dui/" ABSOLUTE)

set(EMBED_RES_DIR "${DUI_ROOT}/resources")
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")
# PolluxOS headers shared by the components (PolluxPaths.h, ...).
include_directories("${CMAKE_CURRENT_LIST_DIR}/../common")
# The Wi-Fi data layer lives with the Wi-Fi window; the shell's popover uses
# it too, so the panel and the window can never disagree about the networks.
include_directories("${CMAKE_CURRENT_LIST_DIR}/../wifi")

include("${DUI_SRC_ROOT_DIR}/cmake/dui_app.cmake")

# The application owns its target: sources are listed, not globbed, and
# dui::app carries the C++20 standard and the Skia usage requirements.
add_executable(${PROJECT_NAME}
    LaunchPadForm.cpp
    PolluxOSForm.cpp
    main.cpp
    ../wifi/WifiData.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE dui::app)
dui_finalize_app(${PROJECT_NAME})
