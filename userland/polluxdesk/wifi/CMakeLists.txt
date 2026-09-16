cmake_minimum_required(VERSION 3.18)

project(polluxdesk_wifi CXX)

# The rest of the dui machinery (dependency setup, Skia) is already configured
# once by the top-level file.
get_filename_component(DUI_SRC_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dui/" ABSOLUTE)

set(EMBED_RES_DIR "${DUI_ROOT}/resources")
# Embed only the desktop's own theme. The platform themes, and the default theme
# a partial theme would otherwise fall back to, are not packaged at all: a
# missing resource must be visible, not quietly taken from another theme.
set(EMBED_RES_PATHS themes/polluxos fonts lang)
include("${DUI_SRC_ROOT_DIR}/cmake/dui_embed_res.cmake")
# PolluxOS headers shared by the components (PolluxPaths.h, PolluxSettings.h).
include_directories("${CMAKE_CURRENT_LIST_DIR}/../common")

include("${DUI_SRC_ROOT_DIR}/cmake/dui_app.cmake")

# The application owns its target: sources are listed, not globbed, and
# dui::app carries the C++20 standard and the Skia usage requirements.
add_executable(${PROJECT_NAME}
    WifiData.cpp
    WifiForm.cpp
    main.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE dui::app)
dui_finalize_app(${PROJECT_NAME})
