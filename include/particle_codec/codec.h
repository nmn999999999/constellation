#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "pseudo_random.h"
#include "grid_mapping.h"
#include "coordinate_encoder.h"
#include "mapping_restorer.h"
#include "frame_builder.h"
#include "frame_parser.h"
#include "hamming.h"

namespace particle_codec {
    class ParticleCodec {
    public:
        // No username: the mapping is derived from a fixed built-in seed input
        // plus the domain, so anyone can decode a particle field.
        ParticleCodec(const std::string &domain = "particle_codec",
                      int gridCols = 60, int gridRows = 60, bool useMicroOffset = false);

        std::vector<EncodedFrame> encode(const std::vector<uint8_t> &data);

        std::vector<EncodedFrame> encodeWithEcc(const std::vector<uint8_t> &data);

        std::optional<std::vector<uint8_t> > decodeCentroids(const std::vector<std::pair<double, double> > &centroids);

        std::optional<std::vector<uint8_t> > decodeCentroidsPayload(
            const std::vector<std::pair<double, double> > &centroids);

        std::optional<std::vector<uint8_t> > decodeCentroidsWithEcc(
            const std::vector<std::pair<double, double> > &centroids);

        void feedFrame(const std::vector<std::pair<double, double> > &centroids);

        void feedRawBytes(const std::vector<uint8_t> &frameBytes);

        std::optional<std::vector<uint8_t> > flushAssembledData();

        std::optional<std::vector<uint8_t> > flushAllData();

        std::vector<ParsedFrame> parseRawBytes(const std::vector<uint8_t> &data);

        int maxPayloadBytes() const { return encoder_.maxPayloadBytes(); }
        int totalCells() const { return gridCols_ * gridRows_; }
        int gridCols() const { return gridCols_; }
        int gridRows() const { return gridRows_; }

        static bool verifyRoundtrip(const std::vector<uint8_t> &data, const std::string &domain = "particle_codec");

    private:
        std::string domain_;
        int gridCols_, gridRows_;
        bool useMicroOffset_;
        std::vector<uint8_t> seed_;
        CoordinateEncoder encoder_;
        MappingRestorer restorer_;
        FrameSplitter splitter_;
        FrameAssembler assembler_;
    };
} // namespace particle_codec
