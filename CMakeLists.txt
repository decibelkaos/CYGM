# CYGM - CGM Display Project
# For JC2432W328 (ESP32 + ST7789 + CST820)

cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Use custom partition table (OTA dual-slot layout)
set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")

# The version the running firmware compares a downloaded image against.
#
# Left unset, ESP-IDF fills the image descriptor from `git describe`, which for
# this repo produces things like "v0.17.0-unreleased-13-g1037e00-". The update
# checker parses that field with "%d.%d.%d" and refuses anything it cannot read,
# so with a git string there every over-the-air install fails as "unreadable
# image version". The one update that did work went onto a v0.16.3 device, which
# predates that check entirely — which is exactly why it was never noticed.
#
# Read from shared_state.h so there is one source of truth and it cannot drift
# from the version on the About screen.
file(READ "${CMAKE_SOURCE_DIR}/main/shared_state.h" _cygm_hdr)
string(REGEX MATCH "CYGM_VERSION_MAJOR[ \t]+([0-9]+)" _m "${_cygm_hdr}")
set(_cygm_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "CYGM_VERSION_MINOR[ \t]+([0-9]+)" _m "${_cygm_hdr}")
set(_cygm_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "CYGM_VERSION_PATCH[ \t]+([0-9]+)" _m "${_cygm_hdr}")
set(_cygm_patch "${CMAKE_MATCH_1}")
if(NOT _cygm_major MATCHES "^[0-9]+$" OR NOT _cygm_minor MATCHES "^[0-9]+$"
   OR NOT _cygm_patch MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "Could not read CYGM_VERSION_MAJOR/MINOR/PATCH from main/shared_state.h. "
        "Building without them would put a git string in the image descriptor and "
        "every over-the-air update would be refused as unreadable.")
endif()
set(PROJECT_VER "${_cygm_major}.${_cygm_minor}.${_cygm_patch}")
message(STATUS "CYGM image version: ${PROJECT_VER}")

project(cygm)

# cJSON recurses once per nesting level and each frame costs ~64 bytes, so a
# deeply nested response overruns the 5 KB glucose task stack long before the
# default limit of 1000 is reached. 16 is above the deepest document any
# provider or the update manifest actually sends.
idf_component_get_property(json_lib json COMPONENT_LIB)
if(json_lib)
    target_compile_definitions(${json_lib} PRIVATE CJSON_NESTING_LIMIT=16)
else()
    message(FATAL_ERROR "json component not in the build; cJSON nesting limit not applied")
endif()

# SPIFFS removed — partition reclaimed for OTA dual-slot layout
