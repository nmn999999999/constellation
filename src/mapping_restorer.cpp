#include "particle_codec/mapping_restorer.h"

#include <stdexcept>

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
        ErrorInfo ignored;
        return restoreFrameEx(centroids, ignored);
    }

    std::optional<std::vector<uint8_t> > MappingRestorer::restoreFrameEx(
        const std::vector<std::pair<double, double> > &centroids, ErrorInfo &outError) {
        outError = ErrorInfo{};
        if (centroids.empty()) {
            outError = makeError(ErrorCode::NoParticles, "no particles supplied for decode");
            return std::nullopt;
        }

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

        if (static_cast<int>(allBytes.size()) < FrameHeader::totalSize) {
            outError = makeError(
                ErrorCode::FrameTooShort,
                "grid capacity (" + std::to_string(allBytes.size()) +
                " bytes) is smaller than a frame header (" +
                std::to_string(FrameHeader::totalSize) + " bytes)");
            return std::nullopt;
        }

        auto header = FrameHeader::tryParse(allBytes);
        if (!header) {
            outError = makeError(ErrorCode::SyncNotFound,
                                 "0xAA55AA55 sync word not found in recovered bits");
            return std::nullopt;
        }

        int totalLen = FrameHeader::totalSize + header->payloadLength;
        if (static_cast<int>(allBytes.size()) < totalLen) {
            outError = makeError(
                ErrorCode::PayloadTooLong,
                "frame declares " + std::to_string(header->payloadLength) +
                " payload bytes but the grid only holds " +
                std::to_string(static_cast<int>(allBytes.size()) - FrameHeader::totalSize));
            return std::nullopt;
        }

        std::vector<uint8_t> frameBytes(allBytes.begin(), allBytes.begin() + totalLen);
        if (!FrameBuilder::verifyCrc(frameBytes)) {
            outError = makeError(ErrorCode::CrcMismatch,
                                 "CRC32 verification failed for recovered frame");
            return std::nullopt;
        }

        return frameBytes;
    }

    std::optional<std::vector<uint8_t> > MappingRestorer::restoreFrameRaw(
        const std::vector<std::pair<double, double> > &centroids) {
        if (centroids.empty()) return std::nullopt;

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

        return std::vector<uint8_t>(allBytes.begin(), allBytes.begin() + totalLen);
    }

    std::optional<std::vector<uint8_t> > MappingRestorer::restoreFrameFromFloat32(
        const std::vector<float> &coords, int count) {
        if (count < 0) {
            throw std::invalid_argument(
                "MappingRestorer::restoreFrameFromFloat32: count must be >= 0, got " +
                std::to_string(count));
        }
        if (static_cast<size_t>(count) * 2 > coords.size()) {
            throw std::invalid_argument(
                "MappingRestorer::restoreFrameFromFloat32: count " + std::to_string(count) +
                " needs " + std::to_string(count * 2) + " coords but only " +
                std::to_string(coords.size()) + " were supplied");
        }
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
