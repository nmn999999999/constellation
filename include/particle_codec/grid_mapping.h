#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace particle_codec {
    struct GridMapping {
        int cols;
        int rows;
        double cellWidth;
        double cellHeight;
        bool useMicroOffset;
        // When true, each cell's centre is shifted to a deterministic
        // pseudo-random spot inside the cell ([0.3, 0.7] of the cell), so the
        // encoded particle field reads as a natural star chart instead of a
        // regular lattice. Decoding still works with plain floor() because the
        // offset is always < 1 cell.
        bool irregularCenters;

        // Throws std::invalid_argument unless cols >= 1 and rows >= 1.
        GridMapping(int cols = 60, int rows = 60, double cellWidth = 1.0, double cellHeight = 1.0,
                    bool useMicroOffset = false, bool irregularCenters = false);

        int totalCells() const { return cols * rows; }
        int bitsPerCell() const { return useMicroOffset ? 2 : 1; }
        int totalBits() const { return totalCells() * bitsPerCell(); }

        std::pair<int, int> bitIndexToGrid(int index) const;

        int gridToBitIndex(int col, int row) const;

        std::pair<double, double> gridToCenter(int col, int row) const;

        std::pair<int, int> centerToGrid(double x, double y) const;

        std::pair<double, double> quantizeOffset(double x, double y, int col, int row) const;

        std::vector<std::pair<int, int> > bitsToGrids(const std::vector<uint8_t> &bits) const;

        std::vector<uint8_t> gridsToBits(const std::vector<std::pair<int, int> > &grids, int length = -1) const;

        static std::vector<uint8_t> bytesToBits(const std::vector<uint8_t> &bytes);

        static std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t> &bits);

        static std::vector<uint8_t> intToBytes(int value, int byteCount);

        static int bytesToInt(const std::vector<uint8_t> &bytes, int offset = 0, int length = -1);

        // Fast mapping between particles and bits (common in encode/decode)
        std::vector<uint8_t> mapParticlesToBits(
            const std::vector<std::pair<double, double> > &centroids,
            const std::vector<int> &invPerm) const;

        std::vector<std::pair<double, double> > mapBitsToParticles(
            const std::vector<uint8_t> &bits, const std::vector<int> &perm) const;

        // Float version for compatibility
        std::vector<uint8_t> mapFloatCoordsToBits(
            const std::vector<float> &coords, int count,
            const std::vector<int> &invPerm) const;
    };
} // namespace particle_codec
