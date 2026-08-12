#include "particle_codec/hamming.h"
#include "particle_codec/grid_mapping.h"

namespace particle_codec {
    std::vector<uint8_t> HammingEncoder::encode(const std::vector<uint8_t> &data) {
        auto inputBits = GridMapping::bytesToBits(data);
        std::vector<uint8_t> outputBits;
        outputBits.reserve((inputBits.size() + dataBits - 1) / dataBits * totalBits);

        for (size_t i = 0; i < inputBits.size(); i += dataBits) {
            std::vector<uint8_t> chunk(dataBits, 0);
            for (int j = 0; j < dataBits; j++)
                if (i + j < inputBits.size()) chunk[j] = inputBits[i + j];
            auto encoded = encodeChunk(chunk);
            outputBits.insert(outputBits.end(), encoded.begin(), encoded.end());
        }
        return GridMapping::bitsToBytes(outputBits);
    }

    std::vector<uint8_t> HammingEncoder::decode(const std::vector<uint8_t> &data, int originalLength) {
        auto inputBits = GridMapping::bytesToBits(data);
        std::vector<uint8_t> dataOutput;
        dataOutput.reserve(inputBits.size() / totalBits * dataBits);

        for (size_t i = 0; i + totalBits <= inputBits.size(); i += totalBits) {
            std::vector<uint8_t> chunk(inputBits.begin() + i, inputBits.begin() + i + totalBits);
            auto decoded = decodeChunk(chunk);
            dataOutput.insert(dataOutput.end(), decoded.begin(), decoded.end());
        }

        auto allBytes = GridMapping::bitsToBytes(dataOutput);
        if (originalLength > 0 && static_cast<int>(allBytes.size()) >= originalLength)
            allBytes.resize(originalLength);
        return allBytes;
    }

    std::vector<uint8_t> HammingEncoder::encodeChunk(const std::vector<uint8_t> &data) {
        std::vector<uint8_t> bits(totalBits, 0);
        bits[2] = data[0];
        bits[4] = data[1];
        bits[5] = data[2];
        bits[6] = data[3];
        bits[0] = bits[2] ^ bits[4] ^ bits[6];
        bits[1] = bits[2] ^ bits[5] ^ bits[6];
        bits[3] = bits[4] ^ bits[5] ^ bits[6];
        return bits;
    }

    std::vector<uint8_t> HammingEncoder::decodeChunk(const std::vector<uint8_t> &bits) {
        auto b = bits;
        int s1 = b[0] ^ b[2] ^ b[4] ^ b[6];
        int s2 = b[1] ^ b[2] ^ b[5] ^ b[6];
        int s3 = b[3] ^ b[4] ^ b[5] ^ b[6];
        int errorPos = s1 + s2 * 2 + s3 * 4;
        if (errorPos > 0 && errorPos <= totalBits)
            b[errorPos - 1] ^= 1;
        return {b[2], b[4], b[5], b[6]};
    }

    bool HammingEncoder::hasError(const std::vector<uint8_t> &bits) {
        return (bits[0] ^ bits[2] ^ bits[4] ^ bits[6]) |
               (bits[1] ^ bits[2] ^ bits[5] ^ bits[6]) |
               (bits[3] ^ bits[4] ^ bits[5] ^ bits[6]);
    }

    int HammingEncoder::correctedErrorCount(const std::vector<uint8_t> &data) {
        auto inputBits = GridMapping::bytesToBits(data);
        int errors = 0;
        for (size_t i = 0; i + totalBits <= inputBits.size(); i += totalBits) {
            std::vector<uint8_t> chunk(inputBits.begin() + i, inputBits.begin() + i + totalBits);
            if (hasError(chunk)) errors++;
        }
        return errors;
    }
} // namespace particle_codec
