#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include "pseudo_random.h"
#include "grid_mapping.h"
#include "frame_builder.h"

namespace particle_codec {
    class MappingRestorer {
    public:
        MappingRestorer(const std::vector<uint8_t> &seed, int gridCols = 60, int gridRows = 60);

        std::optional<std::vector<uint8_t> > restoreFrame(const std::vector<std::pair<double, double> > &centroids);

        std::optional<std::vector<uint8_t> > restoreFrameFromFloat32(const std::vector<float> &coords, int count);

        int extractSeq(const std::vector<std::pair<double, double> > &centroids);

        std::vector<uint8_t> extractPayload(const std::vector<std::pair<double, double> > &centroids);

        bool validateSyncWord(const std::vector<std::pair<double, double> > &centroids);

    private:
        int gridCols_, gridRows_;
        GridMapping grid_;
        std::vector<int> invPerm_;
    };
} // namespace particle_codec
