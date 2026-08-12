#include "particle_codec/mapping_restorer.h"

namespace particle_codec {
    MappingRestorer::MappingRestorer(const std::vector<uint8_t> &seed, int gridCols, int gridRows)
        : gridCols_(gridCols), gridRows_(gridRows),
          grid_(gridCols, gridRows) {
        PseudoRandom prng(seed);
        auto perm = prng.permutation(grid_.totalCells());
        invPerm_ = PseudoRandom::inversePermutation(perm);
    }

    std::optional<std::vector<uint8_t> > MappingRestorer::restoreFrame(
        const std::vector<std::pair<double, double> > &centroids) {
        int byteCount = (grid_.totalCells() + 7) / 8;
        std::vector<uint8_t> allBytes(byteCount, 0);
        for (auto [x, y]: centroids) {
            auto [col, row] = grid_.centerToGrid(x, y);
            int shuffledIdx = grid_.gridToBitIndex(col, row);
            if (shuffledIdx >= 0 && shuffledIdx < grid_.totalCells()) {
                int bitIdx = invPerm_[shuffledIdx];
                allBytes[bitIdx / 8] |= static_cast<uint8_t>(1 << (7 - (bitIdx % 8)));
            }
        }

        if (static_cast<int>(allBytes.size()) < FrameHeader::totalSize) return std::nullopt;

        auto header = FrameHeader::tryParse(allBytes);
        if (!header) return std::nullopt;

        int totalLen = FrameHeader::totalSize + header->payloadLength;
        if (static_cast<int>(allBytes.size()) < totalLen) return std::nullopt;

        std::vector<uint8_t> frameBytes(allBytes.begin(), allBytes.begin() + totalLen);
        if (!FrameBuilder::verifyCrc(frameBytes)) return std::nullopt;

        return frameBytes;
    }

    std::optional<std::vector<uint8_t> > MappingRestorer::restoreFrameFromFloat32(
        const std::vector<float> &coords, int count) {
        int byteCount = (grid_.totalCells() + 7) / 8;
        std::vector<uint8_t> allBytes(byteCount, 0);
        for (int i = 0; i < count; i++) {
            double x = coords[i * 2];
            double y = coords[i * 2 + 1];
            auto [col, row] = grid_.centerToGrid(x, y);
            int shuffledIdx = grid_.gridToBitIndex(col, row);
            if (shuffledIdx >= 0 && shuffledIdx < grid_.totalCells()) {
                int bitIdx = invPerm_[shuffledIdx];
                allBytes[bitIdx / 8] |= static_cast<uint8_t>(1 << (7 - (bitIdx % 8)));
            }
        }

        if (static_cast<int>(allBytes.size()) < FrameHeader::totalSize) return std::nullopt;

        auto header = FrameHeader::tryParse(allBytes);
        if (!header) return std::nullopt;

        int totalLen = FrameHeader::totalSize + header->payloadLength;
        if (static_cast<int>(allBytes.size()) < totalLen) return std::nullopt;

        std::vector<uint8_t> frameBytes(allBytes.begin(), allBytes.begin() + totalLen);
        if (!FrameBuilder::verifyCrc(frameBytes)) return std::nullopt;

        return frameBytes;
    }

    int MappingRestorer::extractSeq(const std::vector<std::pair<double, double> > &centroids) {
        auto frame = restoreFrame(centroids);
        if (!frame) return -1;
        return (frame.value()[4] << 8) | frame.value()[5];
    }

    std::vector<uint8_t> MappingRestorer::extractPayload(const std::vector<std::pair<double, double> > &centroids) {
        auto frame = restoreFrame(centroids);
        if (!frame) return {};
        auto header = FrameHeader::tryParse(frame.value());
        if (!header) return {};
        return std::vector<uint8_t>(
            frame.value().begin() + FrameHeader::totalSize,
            frame.value().begin() + FrameHeader::totalSize + header->payloadLength);
    }

    bool MappingRestorer::validateSyncWord(const std::vector<std::pair<double, double> > &centroids) {
        if (centroids.empty()) return false;
        int byteCount = (grid_.totalCells() + 7) / 8;
        std::vector<uint8_t> allBytes(byteCount, 0);
        for (auto [x, y]: centroids) {
            auto [col, row] = grid_.centerToGrid(x, y);
            int shuffledIdx = grid_.gridToBitIndex(col, row);
            if (shuffledIdx >= 0 && shuffledIdx < grid_.totalCells()) {
                int bitIdx = invPerm_[shuffledIdx];
                allBytes[bitIdx / 8] |= static_cast<uint8_t>(1 << (7 - (bitIdx % 8)));
            }
        }
        if (allBytes.size() < 4) return false;
        return allBytes[0] == 0xAA && allBytes[1] == 0x55 &&
               allBytes[2] == 0xAA && allBytes[3] == 0x55;
    }
} // namespace particle_codec
