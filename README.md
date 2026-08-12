# Constellation

*Data hidden in a starfield.*

A visual data encoding system that maps binary data into particle positions on a grid. Data is encoded as the presence
or absence of particles at specific grid cells, with the grid layout deterministically shuffled based on a fixed
built-in seed. There is no key or username: the mapping is public, so anyone who has the image can decode it.

## How It Works

1. **Fixed Seed** — A deterministic seed is derived from a built-in seed input plus the domain via a multi-round hash.
   The permutation of grid cells is identical for every encoder and decoder, so no username is needed to decode.
2. **Frame Structure** — Data is split into frames with sync word (0xAA55AA55), sequence number, payload length, and
   CRC32 checksum.
3. **Coordinate Encoding** — Each data bit maps to a grid cell via the shuffled permutation. Cells with bit=1 get a
   particle at their center. Perlin noise adds gentle motion for organic visual effect.
4. **Decoding** — Particles are detected (from image or coordinate list), their grid positions are reverse-mapped
   through the inverse permutation, and the original bytes are reconstructed with CRC verification.
5. **Error Correction** — Optional Hamming (7,4) encoding provides single-bit error correction per 4-bit chunk.

## Project Structure

```
├── include/particle_codec/      # Public headers
│   ├── codec.h
│   ├── error.h                  # ErrorCode / ErrorInfo / Result<T>
│   ├── grid_calibrator.h        # Affine grid calibration for photographs
│   ├── frame_builder.h
│   ├── coordinate_encoder.h
│   ├── mapping_restorer.h
│   ├── frame_parser.h
│   ├── pseudo_random.h
│   ├── grid_mapping.h
│   ├── hamming.h
│   └── perlin_noise.h
├── src/                         # Implementations
│   ├── codec.cpp
│   ├── error.cpp
│   ├── grid_calibrator.cpp
│   ├── coordinate_encoder.cpp
│   ├── frame_builder.cpp
│   ├── frame_parser.cpp
│   ├── mapping_restorer.cpp
│   ├── grid_mapping.cpp
│   ├── pseudo_random.cpp
│   ├── perlin_noise.cpp
│   ├── hamming.cpp
│   └── mingw_compat.cpp        # MinGW static-link shim (Windows)
├── demo/
│   ├── main.cpp                 # Console demo (test/encode/visual modes)
│   ├── viewer.cpp               # Win32 GUI particle visualization window (MSVC-only)
│   └── decode_image.cpp         # CLI image decoder (stb_image)
├── tests/
│   ├── test_codec.cpp           # 15 unit tests
│   ├── test_error_safety.cpp    # Validation & error-feedback tests
│   ├── test_grid_calibrator.cpp # Rotation/scale/shift calibration tests
│   ├── test_decode_image.cpp    # End-to-end encode/render/decode
│   ├── test_viewer_decode.cpp   # Viewer-style roundtrip (104/104)
│   ├── test_viewer_export.cpp   # Export-style roundtrip (128/128)
│   ├── test_video_generate.cpp  # Multi-frame animation generator (ffmpeg input)
│   ├── test_video_decode.cpp    # Video-frame decode + multi-frame fusion
│   └── test_precision.cpp       # Precision benchmark (recall/decode stats)
├── CMakeLists.txt               # CMake build
└── README.md
```

## Requirements

- C++20 compatible compiler
- CMake 3.16+

### Windows

- MinGW-w64 (GCC 11+), e.g. the toolchain bundled with JetBrains CLion
- MSVC (Visual Studio 2022) is only needed for the Win32 GUI viewer

### Linux / macOS

- GCC 11+ or Clang 14+ (install via your package manager, e.g. `sudo apt install build-essential cmake` or
  `brew install cmake`)

## Features

- **Open decoding** — No username or secret; anyone who has the image can decode it
- **Deterministic encoding** — Same data = same particle positions
- **CRC32 frame verification** — Detects corrupted frames
- **Multi-frame support** — Splits large data across multiple frames with sequence tracking
- **Perlin noise animation** — Gentle organic motion makes the particle field feel alive
- **Hamming error correction** — Optional single-bit correction for noisy channels
- **Image-based decoding** — Detects particles from RGB/RGBA/grayscale images
- **60×60 grid default** — ~438 bytes payload per frame (adjustable)
- **Win32 GUI viewer** — Real-time particle animation window (ESC to close)
- **Structured error feedback** — Result<T>/ErrorInfo pinpoint the failure stage
  (no particles / sync / CRC / payload too long); legacy APIs expose it via
  `ParticleCodec::lastError()`
- **Fail-fast validation** — invalid grids, >64 KB payloads and out-of-range
  parameters throw descriptive exceptions instead of corrupting data
- **Geometry calibration** — automatically recovers rotated, zoomed or shifted
  photographs by fitting an affine transform to the detected particle grid
  (`GridCalibrator`); the CLI tries it after the axis-aligned methods fail

## Photographs & Video

The default decode path assumes an axis-aligned, full-frame particle field. If
the image is a photograph — rotated, zoomed or shifted — `decode_image`
automatically falls back to `GridCalibrator`, which fits an affine transform
(rotation + scale + translation) to the detected centroids and tries all four
orientation variants.

There is also an ML-assisted decoder (`decode_ml`): a small CNN
(train/detector-only mode, `train/train_detector.py --detector-only`)
classifies the 60×60 cells on the GridCalibrator-rectified image, replacing
the color-threshold heuristic for the bitmap. Export the weights with
`train/export_net.py`, then `build\decode_ml.exe photo.png particle_detector.bin`.
The CNN inference is hand-written C++ (no external ML runtime), keeping the
single-file, no-DLL build.

Video adds redundancy: `test_video_generate` renders a multi-frame animation
(Perlin drift, optional crossfade and Hamming ECC), `ffmpeg` composes it into a
video, and `test_video_decode` decodes every frame, locks the global geometry
from the first calibrated frame, fuses per-sequence centroids (majority
voting), re-calibrates the averaged set, and falls back to ECC correction.

Measured robustness (480×480@30 fps): H.264 CRF ≤ 38 (~127 kb/s) decodes
completely; VP9/AV1 recover the full payload even at ~60 KB for a 3.8-second
video; a 90%-zoomed video with black borders recovers 96/114 frames and the
entire payload via calibration + fusion. Rotated fields are recovered by
multi-frame fusion: intensity-weighted centroids, bundle-adjustment over all
frames, majority voting, and a CRC-guided single/double bit repair that fixes
the last 1–2 residual cell errors.

## Related Work

Constellation belongs to the "dot-pattern" family of visual data encoding, but as of 2026 it has few direct
counterparts on GitHub:

- **Matrix codes (QR, Data Matrix, Aztec)** — the closest mainstream relative: data is encoded by the presence or
  absence of cells on a fixed grid and decoded deterministically. Unlike Constellation they are explicit,
  machine-readable barcodes rather than a concealable visual medium.
- **Dot-pattern steganography research (DPCES)** — academic schemes map characters to randomized dot patterns
  (e.g. *A Randomized Dot Pattern Character Encoding Scheme*, 2023). Conceptually adjacent, but they operate as
  character-substitution tables without frame structure, CRC verification, or image-based blob decoding.
- **Point-cloud / 3D steganography** (e.g. [GS-Hider](https://github.com/xuanyuzhang21/GS-Hider), NeurIPS 2024) —
  hides messages in 3D Gaussian-splatting point clouds, sharing the "data lives in point positions" idea in a 3D
  domain.
- **Mainstream image steganography** ([OpenStego](https://github.com/syvaidya/openstego), StegHide, LSB tools,
  [StegaStamp](https://github.com/tancik/StegaStamp)) — alter pixel values or learn a robust encoding; the carrier is
  a natural image rather than an aesthetic particle field.

What distinguishes Constellation: the carrier *is* the message — a deterministic, key-free particle field that anyone
can decode from the image alone, with CRC32 frame verification and optional Hamming error correction.

## Error Handling

Data-dependent failures are reported through `particle_codec::Result<T>` /
`ErrorInfo` with a stage-specific code (`NoParticles`, `SyncNotFound`,
`PayloadTooLong`, `CrcMismatch`, `FrameDuplicate`, `BufferOverflow`, ...). The
legacy `std::optional`-returning APIs keep their signatures and expose the
reason via `ParticleCodec::lastError()`.

Programmer errors fail fast with descriptive exceptions: invalid grid
dimensions (`std::invalid_argument`), payloads larger than the 16-bit frame
length field (`std::length_error`), and out-of-range byte/chunk parameters
(`std::out_of_range`).

The `decode_image` CLI prints per-method diagnostics on failure (particle
counts and the stage where each method broke) and uses distinct exit codes:
`0` decoded, `1` usage/argument error, `2` I/O error, `3` decode failed,
`4` internal error.

## License

[MIT](LICENSE) — free to use, modify and distribute, including commercially;
just keep the copyright notice.

## Building & Running

### CMake (all platforms)

```bash
cmake -B build -S .
cmake --build build

# Console roundtrip test
build/codec_demo            # build\codec_demo.exe on Windows

# Run unit tests
build/codec_test                # core unit tests
build/error_safety_test         # validation & error-feedback tests
build/test_grid_calibrator      # geometry calibration tests

# CLI image decoder
# Tries color/DT detection first, then automatic geometry calibration.
build/decode_image image.png [grid_cols] [grid_rows]

# Multi-frame video pipeline (requires ffmpeg)
build/test_video_generate build/video_test/anim 1752 6
ffmpeg -framerate 30 -i build/video_test/anim/anim_%03d.bmp \
       -c:v libx264 -pix_fmt yuv420p -crf 23 build/video_test/video.mp4
ffmpeg -i build/video_test/video.mp4 -fps_mode vfr \
       build/video_test/frames/f_%03d.png
build/test_video_decode build/video_test/frames build/video_test/payload.bin
```

On Windows the project is built with MinGW (e.g. the toolchain bundled with JetBrains CLion) plus CMake/Ninja. The C++
runtime and winpthread are linked statically, so the executables are single files that depend only on system DLLs
(KERNEL32.dll, msvcrt.dll; GDI32/USER32 for the GDI test) and run on stock Windows without any extra files. The Win32
GUI viewer (`demo/viewer.cpp`) is MSVC-only and is not built by CMake.
