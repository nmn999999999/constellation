#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include "pseudo_random.h"
#include "grid_mapping.h"
#include "frame_builder.h"

namespace particle_codec {
    class CoordinateEncoder {
    public:
        CoordinateEncoder(const std::vector<uint8_t> &seed, int gridCols = 60, int gridRows = 60,
                          bool useMicroOffset = false);

        int maxPayloadBytes() const;

        std::vector<EncodedFrame> encodeData(const std::vector<uint8_t> &data);

        EncodedFrame encodeSingleFrame(const std::vector<uint8_t> &frameData, double time);

        std::vector<uint8_t> decodeParticles(const std::vector<std::pair<double, double> > &particles,
                                             int expectedFrameLength);

        std::optional<std::vector<uint8_t> > tryDecodeFrame(const std::vector<std::pair<double, double> > &particles);

        int getSeqFromParticles(const std::vector<std::pair<double, double> > &particles);

        std::optional<std::vector<uint8_t> > extractPayloadFromParticles(
            const std::vector<std::pair<double, double> > &particles);

        int gridCols() const { return gridCols_; }
        int gridRows() const { return gridRows_; }

    private:
        int gridCols_, gridRows_;
        bool useMicroOffset_;
        GridMapping grid_;
        PseudoRandom prng_;
        std::vector<int> perm_;
        std::vector<int> invPerm_;
        std::unique_ptr<PerlinNoise> noise_;

        EncodedFrame encodeFrameInternal(const std::vector<uint8_t> &frameData, double time);
    };
} // namespace particle_codec


