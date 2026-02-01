# CYGM - CGM Display Project
# For JC2432W328 (ESP32 + ST7789 + CST820)

cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Use custom partition table (2MB app partition for LVGL)
set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")

project(cygm)

# Generate SPIFFS image from web/ directory
spiffs_create_partition_image(spiffs ${CMAKE_SOURCE_DIR}/web FLASH_IN_PROJECT)
