#!/bin/bash
# Cross-platform build script for Particle Codec (Linux/macOS)

set -e

# Detect platform
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="linux"
    COMPILER="g++"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macos"
    COMPILER="clang++"
elif [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
    PLATFORM="windows"
    COMPILER="g++"
else
    echo "Unsupported platform: $OSTYPE"
    exit 1
fi

echo "Building Particle Codec on $PLATFORM..."

# Create build directory
mkdir -p build

# Configure with CMake
if [ "$PLATFORM" == "windows" ]; then
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
else
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$COMPILER"
fi

# Build
cmake --build build

echo "Build complete!"
echo "Run tests with: ./build/test_precision"
