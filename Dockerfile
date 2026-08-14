# Multi-stage build for Particle Codec
# Supports Linux and macOS (via Rosetta on macOS)

FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    g++-11 \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source code
COPY . .

# Build project
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

# Runtime image
FROM ubuntu:22.04

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Copy build artifacts
COPY --from=builder /build/build/decode_image /usr/local/bin/decode_image
COPY --from=builder /build/build/decode_ml /usr/local/bin/decode_ml

# Make executable
RUN chmod +x /usr/local/bin/decode_image /usr/local/bin/decode_ml

# Set working directory
WORKDIR /data

# Default command
ENTRYPOINT ["decode_image"]
