#pragma once
#include <cstdint>
#include <vector>

namespace particle_codec {
    class HammingEncoder {
    public:
        static constexpr int dataBits = 4;
        static constexpr int parityBits = 3;
        static constexpr int totalBits = 7;

        static std::vector<uint8_t> encode(const std::vector<uint8_t> &data);

        static std::vector<uint8_t> decode(const std::vector<uint8_t> &data, int originalLength = 0);

        static std::vector<uint8_t> encodeChunk(const std::vector<uint8_t> &data);

        // Throws std::invalid_argument if bits holds fewer than 7 bits.
        static std::vector<uint8_t> decodeChunk(const std::vector<uint8_t> &bits);

        // Throws std::invalid_argument if bits holds fewer than 7 bits.
        static bool hasError(const std::vector<uint8_t> &bits);

        static int correctedErrorCount(const std::vector<uint8_t> &data);
    };
} // namespace particle_codec
