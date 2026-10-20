#!/bin/bash

# LinguaArray C++ Build Script

set -e

echo "=== LinguaArray C++ Build Script ==="

# Check basic dependencies
echo "Checking basic dependencies..."

if ! command -v cmake &> /dev/null; then
    echo "Error: CMake not installed"
    exit 1
fi

# Check compiler
if command -v g++ &> /dev/null; then
    CXX_VERSION=$(g++ -dumpversion)
    echo "Using g++ $CXX_VERSION"
elif command -v clang++ &> /dev/null; then
    CXX_VERSION=$(clang++ --version | head -1)
    echo "Using $CXX_VERSION"
else
    echo "Error: C++ compiler not found"
    exit 1
fi

# Create build directory
echo "Creating build directory..."
mkdir -p build
cd build

# Select build type
BUILD_TYPE=${1:-Release}
echo "Build type: $BUILD_TYPE"

# CMake options
CMAKE_OPTIONS="-DCMAKE_BUILD_TYPE=$BUILD_TYPE"

# Static build option
if [ "$2" = "static" ]; then
    echo "Enabling static build"
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DSTATIC_BUILD=ON"
fi

# Run CMake
echo "Configuring project..."
cmake $CMAKE_OPTIONS ..

# Compile
echo "Starting compilation..."
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
make -j$NPROC

echo "=== Build Complete ==="
