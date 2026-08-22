cmake_minimum_required(VERSION 3.16)

# OTA ordering must never depend on `git describe` (especially a dirty tree),
# because the signed manifest parser requires a stable SemVer current version.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER
     LIMIT_COUNT 1)
if(PROJECT_VER STREQUAL "")
    message(FATAL_ERROR "version.txt must contain the Inkloop firmware SemVer")
endif()

# A board is an ESP-IDF component selected at configure time. New hardware is
# added under boards/<name> without forking the Inkloop service/runtime layers.
set(INKLOOP_BOARD "m5_papercolor_c151" CACHE STRING "Inkloop board adapter")
set(ENV{INKLOOP_BOARD_COMPONENT} "${INKLOOP_BOARD}")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/boards/${INKLOOP_BOARD}")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the minimal-SKU dependency solution beside its isolated build artifacts.
# Otherwise the component manager rewrites the C151 lockfile when it removes
# hardware-only managed components from the mock composition.
if(INKLOOP_BOARD STREQUAL "mock_minimal")
    idf_build_set_property(DEPENDENCIES_LOCK
        "${CMAKE_BINARY_DIR}/dependencies.lock")
endif()

project(inkloop_idf)
