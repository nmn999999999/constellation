@echo off
REM Cross-platform build script for Particle Codec (Windows)

setlocal enabledelayedexpansion

echo Building Particle Codec on Windows...

REM Create build directory
if not exist build mkdir build

REM Configure with CMake
echo Configuring with CMake...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

REM Build
echo Building...
cmake --build build

echo Build complete!
echo Run tests with: build\test_precision.exe
