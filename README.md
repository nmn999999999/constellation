# Constellation

*Data hidden in a starfield.*

A visual data encoding system that maps binary data into particle positions on a grid. Data is encoded as the presence
or absence of particles at specific grid cells, with the grid layout deterministically shuffled based on a user-specific
seed. There is no key or username: the mapping is fixed and public, so anyone who has the image can decode it.

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
│   ├── frame_builder.h
│   ├── coordinate_encoder.h
│   ├── mapping_restorer.h
│   ├── frame_parser.h
│   ├── pseudo_random.h
│   ├── grid_mapping.h
│   └── hamming.h
├── src/                         # Implementations
│   ├── codec.cpp
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
│   ├── test_decode_image.cpp    # End-to-end encode/render/decode
│   ├── test_viewer_decode.cpp   # Viewer-style roundtrip (104/104)
│   ├── test_viewer_export.cpp   # Export-style roundtrip (128/128)
│   └── test_precision.cpp       # Precision benchmark (recall/decode stats)
├── CMakeLists.txt               # CMake build
├── AGENTS.md                    # Project instructions
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

## Building & Running

### CMake (all platforms)

```bash
cmake -B build -S .
cmake --build build

# Console roundtrip test
build/codec_demo            # build\codec_demo.exe on Windows

# Run unit tests
build/codec_test            # build\codec_test.exe on Windows

# CLI image decoder
build/decode_image image.png [grid_cols] [grid_rows]
```

On Windows the project is built with MinGW (e.g. the toolchain bundled with JetBrains CLion) plus CMake/Ninja. The C++
runtime and winpthread are linked statically, so the executables are single files that depend only on system DLLs
(KERNEL32.dll, msvcrt.dll; GDI32/USER32 for the GDI test) and run on stock Windows without any extra files. The Win32
GUI viewer (`demo/viewer.cpp`) is MSVC-only and is not built by CMake.
