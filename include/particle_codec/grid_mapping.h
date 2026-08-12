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

        GridMapping(int cols = 60, int rows = 60, double cellWidth = 1.0, double cellHeight = 1.0,
                    bool useMicroOffset = false);

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
    };
} // namespace particle_codec
