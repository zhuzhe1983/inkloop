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

# Hardware memory wiring is part of the board adapter contract.  Keep it out
# of the shared defaults so adding a board with no PSRAM (or Quad PSRAM) cannot
# silently inherit the C151's OPI setup.  Release tooling supplies an explicit,
# already merged defaults file; ordinary builds use the shared + selected-board
# pair assembled here.
if(NOT DEFINED SDKCONFIG_DEFAULTS OR "${SDKCONFIG_DEFAULTS}" STREQUAL "")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/boards/${INKLOOP_BOARD}/sdkconfig.defaults"
        CACHE STRING "Inkloop shared and board-specific sdkconfig defaults")
endif()
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

# Existing sdkconfig files outrank SDKCONFIG_DEFAULTS. Fail closed here so a
# stale build directory can never silently turn C151 OPI PSRAM into Quad mode,
# or leak the C151 memory wiring into a board without PSRAM.
if(INKLOOP_BOARD STREQUAL "m5_papercolor_c151")
    if(NOT CONFIG_SPIRAM OR NOT CONFIG_SPIRAM_MODE_OCT OR
       NOT CONFIG_SPIRAM_SPEED_80M OR NOT CONFIG_SPIRAM_TYPE_AUTO OR
       NOT CONFIG_SPIRAM_BOOT_INIT OR NOT CONFIG_SPIRAM_USE_MALLOC OR
       NOT CONFIG_SPIRAM_MEMTEST OR
       NOT CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL EQUAL 4096 OR
       NOT CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL EQUAL 32768 OR
       NOT CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC OR
       CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC OR
       CONFIG_SPIRAM_MODE_QUAD)
        message(FATAL_ERROR
            "C151 requires 8 MiB Octal PSRAM at 80 MHz; remove the stale sdkconfig and reconfigure")
    endif()
elseif(INKLOOP_BOARD STREQUAL "mock_minimal")
    if(CONFIG_SPIRAM)
        message(FATAL_ERROR
            "mock_minimal must not inherit PSRAM; remove the stale sdkconfig and reconfigure")
    endif()
endif()

# Keep the on-disk format writable by the retained Arduino rollback image.
if(NOT CONFIG_LITTLEFS_MULTIVERSION OR
   NOT CONFIG_LITTLEFS_DISK_VERSION_2_0)
    message(FATAL_ERROR
        "Inkloop requires LittleFS disk version 2.0 for rollback compatibility")
endif()
