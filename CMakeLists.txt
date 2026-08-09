# CYGM - CGM Display Project
# For JC2432W328 (ESP32 + ST7789 + CST820)

cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Use custom partition table (OTA dual-slot layout)
set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")

project(cygm)

# SPIFFS removed — partition reclaimed for OTA dual-slot layout
