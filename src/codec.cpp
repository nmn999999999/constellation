#include "particle_codec/codec.h"
#include <cstring>
#include <stdexcept>
#include <string>

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
        if (gridCols < 1 || gridRows < 1) {
            throw std::invalid_argument(
                "ParticleCodec: grid dimensions must be positive, got " +
                std::to_string(gridCols) + "x" + std::to_string(gridRows));
        }
        if (gridCols * gridRows < FrameHeader::totalSize * 8) {
            throw std::invalid_argument(
                "ParticleCodec: grid " + std::to_string(gridCols) + "x" +
                std::to_string(gridRows) + " has " + std::to_string(gridCols * gridRows) +
                " cells but at least " + std::to_string(FrameHeader::totalSize * 8) +
                " are needed to hold one frame header");
        }
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
        auto result = decodeCentroidsDetailed(centroids);
        if (!result) return std::nullopt;
        return result.takeValue();
    }

    Result<std::vector<uint8_t> > ParticleCodec::decodeCentroidsDetailed(
        const std::vector<std::pair<double, double> > &centroids) {
        ErrorInfo err;
        auto frame = restorer_.restoreFrameEx(centroids, err);
        if (!frame) {
            lastError_ = err;
            return Result<std::vector<uint8_t> >::failure(err);
        }
        lastError_ = ErrorInfo{};
        return Result<std::vector<uint8_t> >::success(std::move(frame.value()));
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::decodeCentroidsPayload(
        const std::vector<std::pair<double, double> > &centroids) {
        auto result = decodeCentroidsDetailed(centroids);
        if (!result) return std::nullopt;
        auto header = FrameHeader::tryParse(result.value());
        if (!header) {
            lastError_ = makeError(ErrorCode::SyncNotFound,
                                   "0xAA55AA55 sync word not found in recovered bits");
            return std::nullopt;
        }
        return std::vector<uint8_t>(
            result.value().begin() + FrameHeader::totalSize,
            result.value().begin() + FrameHeader::totalSize + header->payloadLength);
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::decodeCentroidsWithEcc(
        const std::vector<std::pair<double, double> > &centroids) {
        auto payload = decodeCentroidsPayload(centroids);
        if (!payload) return std::nullopt;
        return HammingEncoder::decode(payload.value());
    }

    void ParticleCodec::feedFrame(const std::vector<std::pair<double, double> > &centroids) {
        feedFrameDetailed(centroids);
    }

    Result<void> ParticleCodec::feedFrameDetailed(
        const std::vector<std::pair<double, double> > &centroids) {
        ErrorInfo err;
        auto frame = restorer_.restoreFrameEx(centroids, err);
        if (!frame) {
            lastError_ = err;
            return Result<void>::failure(err);
        }
        auto header = FrameHeader::tryParse(frame.value());
        if (!header) {
            lastError_ = makeError(ErrorCode::SyncNotFound,
                                   "0xAA55AA55 sync word not found in recovered bits");
            return Result<void>::failure(lastError_);
        }
        std::vector<uint8_t> payload(
            frame.value().begin() + FrameHeader::totalSize,
            frame.value().begin() + FrameHeader::totalSize + header.value().payloadLength);
        auto added = assembler_.addFrameEx(header.value().seq, payload);
        lastError_ = added.ok() ? ErrorInfo{} : added.error();
        return added;
    }

    void ParticleCodec::feedRawBytes(const std::vector<uint8_t> &frameBytes) {
        auto header = FrameHeader::tryParse(frameBytes);
        if (!header) {
            lastError_ = makeError(ErrorCode::SyncNotFound,
                                   "0xAA55AA55 sync word not found in frame bytes");
            return;
        }
        std::vector<uint8_t> payload(
            frameBytes.begin() + FrameHeader::totalSize,
            frameBytes.begin() + FrameHeader::totalSize + header.value().payloadLength);
        auto added = assembler_.addFrameEx(header.value().seq, payload);
        lastError_ = added.ok() ? ErrorInfo{} : added.error();
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::flushAssembledData() {
        auto data = assembler_.extractData();
        if (!data) {
            lastError_ = makeError(
                ErrorCode::DecodeFailed,
                "no contiguous assembled data available (frames missing or out of order)");
        } else {
            lastError_ = ErrorInfo{};
        }
        return data;
    }

    std::optional<std::vector<uint8_t> > ParticleCodec::flushAllData() {
        auto data = assembler_.extractAll();
        if (!data) {
            lastError_ = makeError(ErrorCode::DecodeFailed, "assembler buffer is empty");
        } else {
            lastError_ = ErrorInfo{};
        }
        return data;
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
