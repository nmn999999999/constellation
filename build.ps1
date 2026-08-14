# Cross-platform build script for Particle Codec (PowerShell)

Write-Host "Building Particle Codec..." -ForegroundColor Cyan

# Create build directory
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Configure with CMake
Write-Host "Configuring with CMake..." -ForegroundColor Yellow
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build
Write-Host "Building..." -ForegroundColor Yellow
cmake --build build

Write-Host "Build complete!" -ForegroundColor Green
Write-Host "Run tests with: .\build\test_precision.exe" -ForegroundColor Green
