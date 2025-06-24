@echo off
if not exist libs (
    mkdir libs
)

@REM del /q /f libs\* >nul 2>&1
rmdir /s /q CMakeFiles >nul 2>&1
del /q /f cmake_install.cmake >nul 2>&1
del /q /f src/CMakeCache.txt >nul 2>&1
rmdir /q /s src/CMakeFiles >nul 2>&1
del /q /f Makefile >nul 2>&1
del /q /f libWatcherPlus.dll.a >nul 2>&1
del /q /f .ninja_deps >nul 2>&1
del /q /f .ninja_log >nul 2>&1
del /q /f build.ninja >nul 2>&1
del /q /f libWatcherPlus_*.dll.a >nul 2>&1