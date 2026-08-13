cmake_minimum_required(VERSION 3.16)
project(particle_codec VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MINGW)
    # Single-file executables: statically link the MinGW C++ runtime
    # (libgcc/libstdc++/winpthread) so binaries depend only on the system
    # DLLs KERNEL32.dll and msvcrt.dll.
    set(MINGW_COMPAT_SRC src/mingw_compat.cpp)
    add_link_options(-static-libgcc -static-libstdc++)
    # winpthread must be scanned after libstdc++ has pulled the pthread symbols
    # used by iostreams; pass these via link libraries (placed after the objects)
    # rather than link options (placed before them).
    set(MINGW_PTHREAD_LINK -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic)
else ()
    set(MINGW_COMPAT_SRC "")
    set(MINGW_PTHREAD_LINK "")
endif ()

add_library(particle_codec
        src/error.cpp
        src/grid_calibrator.cpp
        src/homography_calibrator.cpp
        src/pseudo_random.cpp
        src/perlin_noise.cpp
        src/grid_mapping.cpp
        src/frame_builder.cpp
        src/coordinate_encoder.cpp
        src/mapping_restorer.cpp
        src/frame_parser.cpp
        src/codec.cpp
        src/hamming.cpp
        src/particle_net.cpp
)

target_include_directories(particle_codec PUBLIC include)

# Console demo
add_executable(codec_demo demo/main.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(codec_demo PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Tests
add_executable(codec_test tests/test_codec.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(codec_test PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Error-safety and error-feedback tests
add_executable(error_safety_test tests/test_error_safety.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(error_safety_test PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Grid geometry calibration tests (rotation/scale/shift recovery)
add_executable(test_grid_calibrator tests/test_grid_calibrator.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_grid_calibrator PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Perspective (homography) calibration test
add_executable(test_homography tests/test_homography.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_homography PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# CLI image decoder
add_executable(decode_image demo/decode_image.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(decode_image PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Hybrid ML decoder (GridCalibrator geometry + trained CNN detection)
add_executable(decode_ml demo/decode_ml.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(decode_ml PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Standalone end-to-end tests (Windows, render BMP then decode)
add_executable(test_decode_image tests/test_decode_image.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_decode_image PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

add_executable(test_viewer_decode tests/test_viewer_decode.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_viewer_decode PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

add_executable(test_viewer_export tests/test_viewer_export.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_viewer_export PRIVATE particle_codec gdi32 user32 ${MINGW_PTHREAD_LINK})

# Multi-frame animation generation + video-frame decode test
add_executable(test_video_generate tests/test_video_generate.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_video_generate PRIVATE particle_codec gdi32 user32 ${MINGW_PTHREAD_LINK})

add_executable(test_video_decode tests/test_video_decode.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_video_decode PRIVATE particle_codec ${MINGW_PTHREAD_LINK})

# Precision benchmark
add_executable(test_precision tests/test_precision.cpp ${MINGW_COMPAT_SRC})
target_link_libraries(test_precision PRIVATE particle_codec ${MINGW_PTHREAD_LINK})


# Win32 GUI viewer (demo/viewer.cpp) is MSVC-only and has no CMake target.
# Build it manually in an MSVC environment if needed.
