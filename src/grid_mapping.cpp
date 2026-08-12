#include "particle_codec/grid_mapping.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cmath>

namespace particle_codec {
    GridMapping::GridMapping(int cols, int rows, double cellWidth, double cellHeight, bool useMicroOffset)
        : cols(cols), rows(rows), cellWidth(cellWidth), cellHeight(cellHeight), useMicroOffset(useMicroOffset) {
        if (cols < 1 || rows < 1) {
            throw std::invalid_argument(
                "GridMapping: grid dimensions must be positive, got " +
                std::to_string(cols) + "x" + std::to_string(rows));
        }
    }

    std::pair<int, int> GridMapping::bitIndexToGrid(int index) const {
        return {index % cols, index / cols};
    }

    int GridMapping::gridToBitIndex(int col, int row) const {
        return row * cols + col;
    }

    std::pair<double, double> GridMapping::gridToCenter(int col, int row) const {
        return {col * cellWidth + cellWidth * 0.5, row * cellHeight + cellHeight * 0.5};
    }

    std::pair<int, int> GridMapping::centerToGrid(double x, double y) const {
        int c = static_cast<int>(std::floor(x));
        int r = static_cast<int>(std::floor(y));
        c = std::clamp(c, 0, cols - 1);
        r = std::clamp(r, 0, rows - 1);
        return {c, r};
    }

    std::pair<double, double> GridMapping::quantizeOffset(double x, double y, int col, int row) const {
        if (!useMicroOffset) return {0.0, 0.0};
        auto [cx, cy] = gridToCenter(col, row);
        double rawDx = x - cx;
        double rawDy = y - cy;
        double maxOffset = cellWidth * 0.35;
        double qDx = std::clamp(rawDx, -maxOffset, maxOffset);
        double qDy = std::clamp(rawDy, -maxOffset, maxOffset);
        double normDx = (qDx + maxOffset) / (2 * maxOffset);
        double normDy = (qDy + maxOffset) / (2 * maxOffset);
        int qLevels = 4;
        int qiDx = std::clamp(static_cast<int>(normDx * qLevels), 0, qLevels - 1);
        int qiDy = std::clamp(static_cast<int>(normDy * qLevels), 0, qLevels - 1);
        double deqDx = (static_cast<double>(qiDx) / (qLevels - 1)) * 2 * maxOffset - maxOffset;
        double deqDy = (static_cast<double>(qiDy) / (qLevels - 1)) * 2 * maxOffset - maxOffset;
        return {deqDx, deqDy};
    }

    std::vector<std::pair<int, int> > GridMapping::bitsToGrids(const std::vector<uint8_t> &bits) const {
        std::vector<std::pair<int, int> > grids;
        int limit = std::min(static_cast<int>(bits.size()), totalCells());
        for (int i = 0; i < limit; i++)
            if (bits[i] == 1) grids.push_back(bitIndexToGrid(i));
        return grids;
    }

    std::vector<uint8_t> GridMapping::gridsToBits(const std::vector<std::pair<int, int> > &grids, int length) const {
        int n = (length < 0) ? totalCells() : length;
        std::vector<uint8_t> bits(n, 0);
        for (auto [col, row]: grids) {
            int idx = gridToBitIndex(col, row);
            if (idx >= 0 && idx < n) bits[idx] = 1;
        }
        return bits;
    }

    std::vector<uint8_t> GridMapping::bytesToBits(const std::vector<uint8_t> &bytes) {
        std::vector<uint8_t> bits(bytes.size() * 8);
        for (size_t i = 0; i < bytes.size(); i++)
            for (int j = 0; j < 8; j++)
                bits[i * 8 + j] = (bytes[i] >> (7 - j)) & 1;
        return bits;
    }

    std::vector<uint8_t> GridMapping::bitsToBytes(const std::vector<uint8_t> &bits) {
        size_t byteCount = (bits.size() + 7) / 8;
        std::vector<uint8_t> bytes(byteCount, 0);
        for (size_t i = 0; i < bits.size(); i++)
            if (bits[i] == 1) bytes[i / 8] |= 1 << (7 - (i % 8));
        return bytes;
    }

    std::vector<uint8_t> GridMapping::intToBytes(int value, int byteCount) {
        if (byteCount < 0 || byteCount > 4) {
            throw std::invalid_argument(
                "GridMapping::intToBytes: byteCount must be in [0, 4], got " +
                std::to_string(byteCount));
        }
        std::vector<uint8_t> bytes(byteCount);
        for (int i = byteCount - 1; i >= 0; i--) {
            bytes[i] = value & 0xFF;
            value >>= 8;
        }
        return bytes;
    }

    int GridMapping::bytesToInt(const std::vector<uint8_t> &bytes, int offset, int length) {
        if (offset < 0 || offset > static_cast<int>(bytes.size())) {
            throw std::out_of_range(
                "GridMapping::bytesToInt: offset " + std::to_string(offset) +
                " out of range [0, " + std::to_string(bytes.size()) + "]");
        }
        int end = (length < 0) ? static_cast<int>(bytes.size()) : offset + length;
        if (end > static_cast<int>(bytes.size())) {
            throw std::out_of_range(
                "GridMapping::bytesToInt: range [" + std::to_string(offset) + ", " +
                std::to_string(end) + ") exceeds buffer size " + std::to_string(bytes.size()));
        }
        int value = 0;
        for (int i = offset; i < end; i++)
            value = (value << 8) | bytes[i];
        return value;
    }
} // namespace particle_codec
