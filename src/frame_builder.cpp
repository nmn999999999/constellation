#include "particle_codec/frame_builder.h"
#include <cstring>

namespace particle_codec {
    uint32_t CRC32::table_[256];
    static bool crcTableInitialized_ = false;

    static void ensureTable() {
        if (crcTableInitialized_) return;
        crcTableInitialized_ = true;
        for (int i = 0; i < 256; i++) {
            uint32_t crc = static_cast<uint32_t>(i);
            for (int j = 0; j < 8; j++)
                crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
            CRC32::table_[i] = crc;
        }
    }

    uint32_t CRC32::update(uint32_t crc, const uint8_t *data, size_t length) {
        ensureTable();
        for (size_t i = 0; i < length; i++)
            crc = table_[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    uint32_t CRC32::calculate(const std::vector<uint8_t> &data) {
        return update(0xFFFFFFFF, data.data(), data.size()) ^ 0xFFFFFFFF;
    }

    std::vector<uint8_t> CRC32::append(const std::vector<uint8_t> &data) {
        uint32_t crc = calculate(data);
        std::vector<uint8_t> result(data.size() + 4);
        std::copy(data.begin(), data.end(), result.begin());
        result[data.size()] = (crc >> 24) & 0xFF;
        result[data.size() + 1] = (crc >> 16) & 0xFF;
        result[data.size() + 2] = (crc >> 8) & 0xFF;
        result[data.size() + 3] = crc & 0xFF;
        return result;
    }

    bool CRC32::verify(const std::vector<uint8_t> &dataWithCRC) {
        if (dataWithCRC.size() < 4) return false;
        uint32_t stored = (dataWithCRC[dataWithCRC.size() - 4] << 24) |
                          (dataWithCRC[dataWithCRC.size() - 3] << 16) |
                          (dataWithCRC[dataWithCRC.size() - 2] << 8) |
                          dataWithCRC[dataWithCRC.size() - 1];
        size_t len = dataWithCRC.size() - 4;
        uint32_t crc = update(0xFFFFFFFF, dataWithCRC.data(), len) ^ 0xFFFFFFFF;
        return crc == stored;
    }

    // FrameHeader
    std::vector<uint8_t> FrameHeader::toBytes() const {
        std::vector<uint8_t> d(totalSize);
        d[0] = 0xAA;
        d[1] = 0x55;
        d[2] = 0xAA;
        d[3] = 0x55;
        d[4] = (seq >> 8) & 0xFF;
        d[5] = seq & 0xFF;
        d[6] = (payloadLength >> 8) & 0xFF;
        d[7] = payloadLength & 0xFF;
        d[8] = 0;
        d[9] = 0;
        d[10] = 0;
        d[11] = 0;
        return d;
    }

    std::optional<FrameHeader> FrameHeader::tryParse(const std::vector<uint8_t> &data) {
        if (data.size() < totalSize) return std::nullopt;
        if (data[0] != 0xAA || data[1] != 0x55 || data[2] != 0xAA || data[3] != 0x55)
            return std::nullopt;
        int seq = (data[4] << 8) | data[5];
        int len = (data[6] << 8) | data[7];
        return FrameHeader(seq, len);
    }

    // FrameBuilder
    int FrameBuilder::maxPayloadBytes(int gridCells) {
        return (gridCells - FrameHeader::totalSize * 8) / 8;
    }

    std::vector<uint8_t> FrameBuilder::build(int seq, const std::vector<uint8_t> &payload) {
        FrameHeader header(seq, static_cast<int>(payload.size()));
        auto headerBytes = header.toBytes();

        uint32_t crc = CRC32::update(
            0xFFFFFFFF, headerBytes.data(), FrameHeader::totalSize - FrameHeader::crcSize);
        crc = CRC32::update(crc, payload.data(), payload.size()) ^ 0xFFFFFFFF;
        headerBytes[8] = (crc >> 24) & 0xFF;
        headerBytes[9] = (crc >> 16) & 0xFF;
        headerBytes[10] = (crc >> 8) & 0xFF;
        headerBytes[11] = crc & 0xFF;

        std::vector<uint8_t> frame(FrameHeader::totalSize + payload.size());
        std::copy(headerBytes.begin(), headerBytes.end(), frame.begin());
        std::copy(payload.begin(), payload.end(), frame.begin() + FrameHeader::totalSize);
        return frame;
    }

    bool FrameBuilder::verifyCrc(const std::vector<uint8_t> &frame) {
        if (frame.size() < FrameHeader::totalSize) return false;
        uint32_t stored = (frame[8] << 24) | (frame[9] << 16) | (frame[10] << 8) | frame[11];
        int pLen = (frame[6] << 8) | frame[7];
        size_t expectedLen = FrameHeader::totalSize + pLen;
        if (frame.size() < expectedLen) return false;

        uint32_t crc = CRC32::update(
            0xFFFFFFFF, frame.data(), FrameHeader::totalSize - FrameHeader::crcSize);
        crc = CRC32::update(crc, frame.data() + FrameHeader::totalSize,
                            expectedLen - FrameHeader::totalSize) ^ 0xFFFFFFFF;
        return crc == stored;
    }
} // namespace particle_codec
