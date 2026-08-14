# Troubleshooting Guide

This guide helps you resolve common issues when building, running, or using Particle Codec.

---

## Build Issues

### 1. CMake not found on Windows

**Problem:** `cmake: command not found` error

**Solution:**
- Install CMake via Chocolatey: `choco install cmake ninja`
- Or download from https://cmake.org/download/
- Ensure CMake is added to PATH

### 2. Ninja not found on Windows

**Problem:** `ninja: command not found` error

**Solution:**
- Install Ninja via Chocolatey: `choco install ninja`
- Or download from https://github.com/ninja-build/ninja/releases

### 3. MinGW not found on Linux/macOS

**Problem:** `g++: command not found` error

**Solution:**
- Ubuntu/Debian: `sudo apt-get install g++-11 cmake ninja-build`
- macOS: `brew install cmake ninja`
- Arch Linux: `sudo pacman -S base-devel cmake ninja`

### 4. Static runtime linking issues

**Problem:** Missing `libgcc_s_dw2-1.dll` or similar MinGW DLLs at runtime

**Solution:**
- Use the provided `build.bat` script which automatically links statically
- Or run: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"`

---

## Runtime Issues

### 1. "Not a Constellation field" error

**Problem:** `decode_image` reports no particles found

**Possible causes:**
- **Wrong image:** The image must be a Particle Codec export with cyan particles (R<80, G>100, B>100, B>R+20, G>R+20)
- **Old format:** Old viewer exports (pr=1.5*scale=12) have overlapping particles and are unreadable
- **Wrong grid size:** Default is 60×60; try `decode_image.exe image.png 120 120`

**Solution:**
- Re-export the image using the latest viewer (pr=0.35*scale=2)
- Try different grid sizes: `decode_image.exe image.png 80 80`
- Check the image manually: look for cyan dots on a dark background

### 2. SyncNotFound error

**Problem:** `decode_image` reports sync word not found

**Possible causes:**
- Image is heavily compressed (JPEG CRF > 42)
- Image is resized or cropped (field outside frame)
- Wrong grid size or rotation

**Solution:**
- Use VP9/AV1 with CRF ≤ 50 for videos
- For images, use PNG or JPEG CRF ≤ 38
- Verify the image shows the full particle field (no black borders)
- Try geometry calibration: `decode_image.exe image.png 60 60` (auto-calibrates)

### 3. CrcMismatch error

**Problem:** `decode_image` reports CRC mismatch

**Possible causes:**
- Image quality too low (blur, noise, compression)
- Wrong grid size
- Corrupted image (partial damage)

**Solution:**
- Improve image quality (higher resolution, lower compression)
- Try different decode methods (the CLI tries 4 methods automatically)
- For videos, use multi-frame fusion: `test_video_decode.exe frames payload.bin`
- Check the image manually for visible particle damage

### 4. decode_image.exe not found

**Problem:** Command not recognized

**Solution:**
- Run from the project root directory
- Use `.\build\decode_image.exe` (Windows) or `./build/decode_image` (Linux/macOS)
- Verify the build succeeded: `cmake --build build`

---

## Test Failures

### 1. codec_test.exe fails

**Problem:** Unit tests report failures

**Solution:**
- Rebuild from scratch: `rm -rf build && cmake -S . -B build -G Ninja && cmake --build build`
- Check that all source files are present in `src/` and `tests/`
- Verify C++20 support: `g++ --version` should show ≥ 11

### 2. test_grid_calibrator.exe fails

**Problem:** Geometry calibration tests fail

**Possible causes:**
- Floating-point precision issues (unlikely with C++17)
- Compiler optimization bug

**Solution:**
- Build in Debug mode: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`
- Rebuild and rerun tests

### 3. test_precision.exe crashes

**Problem:** Benchmark crashes or produces wrong results

**Solution:**
- Verify the build is successful
- Check that all tests pass before running the benchmark
- Report the issue with output logs

---

## ML Training Issues

### 1. Python environment not found

**Problem:** `ModuleNotFoundError: No module named 'torch'`

**Solution:**
```bash
pip install torch numpy pillow
```

For CPU-only build (faster on Windows):
```bash
pip install torch --index-url https://download.pytorch.org/whl/cpu
```

### 2. CUDA not found

**Problem:** PyTorch complains about CUDA not available

**Solution:**
- Install CUDA toolkit and cuDNN (for GPU training)
- Or use CPU-only build: `pip install torch --index-url https://download.pytorch.org/whl/cpu`

### 3. Export network weights

**Problem:** `export_net.py` fails with "missing keys"

**Solution:**
- Verify the checkpoint file exists: `particle_detector.pt`
- Check that the model structure matches the expected keys in `export_net.py`
- Train the model first: `python train_detector.py --epochs 20`

---

## Performance Issues

### 1. decode_image is slow

**Problem:** Decoding takes too long

**Solution:**
- The CLI is already optimized (C++ implementation)
- For large images, try using the distance transform method (faster than color flood-fill)
- For videos, use `test_video_decode` which uses multi-frame fusion

### 2. Memory usage high

**Problem:** decode_image uses excessive memory

**Solution:**
- The decoder loads the entire image into memory
- For very large images (> 4K), consider downsampling first
- The ML decoder (`decode_ml`) may use more memory due to ONNX runtime

---

## Getting Help

If you've tried the solutions above and still have issues:

1. Check the [README.md](README.md) for feature documentation
2. Check the [AGENTS.md](AGENTS.md) for technical details
3. Run tests with verbose output: `build/test_precision.exe --verbose`
4. Collect logs and error messages
5. Open an issue on GitHub with:
   - Platform (Windows/Linux/macOS)
   - Compiler version
   - CMake version
   - Full error message
   - Steps to reproduce
