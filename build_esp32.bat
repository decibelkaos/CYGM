@echo off
REM ESP32 Build Script for CYGM Project
REM This script sets up the ESP-IDF environment and builds the project

echo Setting up ESP-IDF environment...
call "C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat"

echo.
echo Building CYGM project...
cd /d "C:\Projects\CYGM"
idf.py build

echo.
echo Build complete! Check above for any errors.
pause
