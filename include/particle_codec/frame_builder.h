#pragma once
#include <cstdint>
#include <vector>
#include <optional>

namespace particle_codec {
    // Frame header constants
    struct FrameHeader {
        static constexpr int syncSize = 4;
        static constexpr int seqSize = 2;
        static constexpr int lenSize = 2;
        static constexpr int crcSize = 4;
        static constexpr int totalSize = syncSize + seqSize + lenSize + crcSize;
        static constexpr uint32_t syncWord = 0xAA55AA55;

        int seq;
        int payloadLength;

        FrameHeader() : seq(0), payloadLength(0) {
        }

        FrameHeader(int seq, int payloadLength) : seq(seq), payloadLength(payloadLength) {
        }

        std::vector<uint8_t> toBytes() const;

        static std::optional<FrameHeader> tryParse(const std::vector<uint8_t> &data);
    };

    // CRC32 implementation
    class CRC32 {
    public:
        static uint32_t calculate(const std::vector<uint8_t> &data);

        // Incremental CRC over a raw span (used to avoid copying frame slices).
        static uint32_t update(uint32_t crc, const uint8_t *data, size_t length);

        static std::vector<uint8_t> append(const std::vector<uint8_t> &data);

        static bool verify(const std::vector<uint8_t> &dataWithCRC);

        static uint32_t table_[256];
    };

    // Frame builder
    class FrameBuilder {
    public:
        static int maxPayloadBytes(int gridCells = 60 * 60);

        static std::vector<uint8_t> build(int seq, const std::vector<uint8_t> &payload);

        static bool verifyCrc(const std::vector<uint8_t> &frame);
    };

    // Encoded frame data
    struct EncodedFrame {
        int seq;
        std::vector<float> particles; // interleaved x, y
        int particleCount;
        int frameByteLength;

        EncodedFrame() : seq(0), particleCount(0), frameByteLength(0) {
        }

        EncodedFrame(int seq, std::vector<float> particles, int count, int byteLen)
            : seq(seq), particles(std::move(particles)), particleCount(count), frameByteLength(byteLen) {
        }
    };
} // namespace particle_codec
