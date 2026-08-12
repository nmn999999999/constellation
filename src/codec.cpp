#include "particle_codec/codec.h"
#include <cstring>

namespace particle_codec {
    namespace {
        // Fixed seed input for every codec instance. The username concept was
        // removed so that a particle field can be decoded without any secret.
        // It intentionally matches the old default user ("demo_user") so
        // images exported earlier with the default username still decode.
        const char kSeedInput[] = "demo_user";
    }

    ParticleCodec::ParticleCodec(const std::string &domain,
                                 int gridCols, int gridRows, bool useMicroOffset)
        : domain_(domain),
          gridCols_(gridCols), gridRows_(gridRows), useMicroOffset_(useMicroOffset),
          seed_(PseudoRandom::deriveSeed(kSeedInput, domain)),
          encoder_(seed_, gridCols, gridRows, useMicroOffset),
          restorer_(seed_, gridCols, gridRows),
          splitter_(encoder_.maxPayloadBytes()),
          assembler_(256) {
    }

    std::vector<EncodedFrame> ParticleCodec::encode(const std::vector<uint8_t> &data) {
        return encoder_.encodeData(data);
    }

    std::vector<EncodedFrame> ParticleCodec::encodeWithEcc(const std::vector<uint8_t> &data) {
        auto encoded = HammingEncoder::encode(data);
        return encoder_.encodeData(encoded);
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::decodeCentroids(
        const std::vector<std::pair<double, double> > &centroids) {
        return restorer_.restoreFrame(centroids);
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::decodeCentroidsPayload(
        const std::vector<std::pair<double, double> > &centroids) {
        return encoder_.extractPayloadFromParticles(centroids);
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::decodeCentroidsWithEcc(
        const std::vector<std::pair<double, double> > &centroids) {
        auto payload = encoder_.extractPayloadFromParticles(centroids);
        if (!payload) return std::nullopt;
        return HammingEncoder::decode(payload.value());
    }

    void ParticleCodec::feedFrame(const std::vector<std::pair<double, double> > &centroids) {
        auto frame = restorer_.restoreFrame(centroids);
        if (!frame) return;
        auto header = FrameHeader::tryParse(frame.value());
        if (!header) return;
        std::vector<uint8_t> payload(
            frame.value().begin() + FrameHeader::totalSize,
            frame.value().begin() + FrameHeader::totalSize + header.value().payloadLength);
        assembler_.addFrame(header.value().seq, payload);
    }

    void ParticleCodec::feedRawBytes(const std::vector<uint8_t> &frameBytes) {
        auto header = FrameHeader::tryParse(frameBytes);
        if (!header) return;
        std::vector<uint8_t> payload(
            frameBytes.begin() + FrameHeader::totalSize,
            frameBytes.begin() + FrameHeader::totalSize + header.value().payloadLength);
        assembler_.addFrame(header.value().seq, payload);
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::flushAssembledData() {
        return assembler_.extractData();
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::flushAllData() {
        return assembler_.extractAll();
    }

    std::vector<ParsedFrame> ParticleCodec::parseRawBytes(const std::vector<uint8_t> &data) {
        return FrameParser().parseStream(data);
    }

    bool ParticleCodec::verifyRoundtrip(const std::vector<uint8_t> &data, const std::string &domain) {
        ParticleCodec codec(domain);
        auto frames = codec.encode(data);

        FrameAssembler assembler;
        for (const auto &frame: frames) {
            std::vector<std::pair<double, double> > centroids;
            for (int i = 0; i < frame.particleCount; i++)
                centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);

            auto restored = codec.decodeCentroids(centroids);
            if (!restored) return false;

            auto header = FrameHeader::tryParse(restored.value());
            if (!header) return false;

            std::vector<uint8_t> payload(
                restored.value().begin() + FrameHeader::totalSize,
                restored.value().begin() + FrameHeader::totalSize + header.value().payloadLength);
            assembler.addFrame(header.value().seq, payload);
        }

        auto result = assembler.extractData();
        if (!result) return false;
        if (result->size() != data.size()) return false;
        return memcmp(result->data(), data.data(), data.size()) == 0;
    }
} // namespace particle_codec
