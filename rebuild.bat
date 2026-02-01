@echo off
call "C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat"
cd /d "C:\Projects\CYGM"
idf.py build 2>&1 | tee build_output.log
