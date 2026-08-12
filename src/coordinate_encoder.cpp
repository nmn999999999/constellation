#include "particle_codec/coordinate_encoder.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace particle_codec {
    CoordinateEncoder::CoordinateEncoder(const std::vector<uint8_t> &seed, int gridCols, int gridRows,
                                         bool useMicroOffset)
        : gridCols_(gridCols), gridRows_(gridRows), useMicroOffset_(useMicroOffset),
          grid_(gridCols, gridRows, 1.0, 1.0, useMicroOffset),
          prng_(seed) {
        perm_ = prng_.permutation(grid_.totalCells());
        invPerm_ = PseudoRandom::inversePermutation(perm_);

        std::vector<uint8_t> noiseSeed(32);
        for (int i = 0; i < 32; i++) noiseSeed[i] = seed[i] ^ 0x55;
        noise_ = std::make_unique<PerlinNoise>(noiseSeed);
    }

    int CoordinateEncoder::maxPayloadBytes() const {
        return (grid_.totalCells() - FrameHeader::totalSize * 8) / 8;
    }

    std::vector<EncodedFrame> CoordinateEncoder::encodeData(const std::vector<uint8_t> &data) {
        std::vector<EncodedFrame> frames;
        int chunkSize = maxPayloadBytes();
        if (chunkSize <= 0) {
            throw std::length_error(
                "CoordinateEncoder::encodeData: grid " + std::to_string(gridCols_) + "x" +
                std::to_string(gridRows_) + " is too small to hold any frame payload");
        }

        for (int offset = 0; offset < static_cast<int>(data.size()); offset += chunkSize) {
            int end = std::min(offset + chunkSize, static_cast<int>(data.size()));
            std::vector<uint8_t> chunk(data.begin() + offset, data.begin() + end);
            int seq = static_cast<int>(frames.size());
            auto frameBytes = FrameBuilder::build(seq, chunk);
            frames.push_back(encodeFrameInternal(frameBytes, 0.0));
        }

        if (frames.empty()) {
            auto empty = FrameBuilder::build(0, {});
            frames.push_back(encodeFrameInternal(empty, 0.0));
        }

        return frames;
    }

    EncodedFrame CoordinateEncoder::encodeSingleFrame(const std::vector<uint8_t> &frameData, double time) {
        return encodeFrameInternal(frameData, time);
    }

    EncodedFrame CoordinateEncoder::encodeFrameInternal(const std::vector<uint8_t> &frameData, double time) {
        // Write the frame bits directly into a per-cell array (1 byte/bit),
        // skipping the intermediate bytesToBits() expansion.
        std::vector<uint8_t> bits(grid_.totalCells(), 0);
        size_t bitCount = frameData.size() * 8;
        for (size_t i = 0; i < bitCount && i < static_cast<size_t>(grid_.totalCells()); i++)
            bits[i] = (frameData[i / 8] >> (7 - (i % 8))) & 1;

        int count = static_cast<int>(std::count(bits.begin(), bits.end(), 1));
        std::vector<float> coords(count * 2);
        int idx = 0;

        for (int i = 0; i < grid_.totalCells(); i++) {
            if (bits[i] == 1) {
                int shuffledIdx = perm_[i];
                auto [col, row] = grid_.bitIndexToGrid(shuffledIdx);
                auto [cx, cy] = grid_.gridToCenter(col, row);

                double nx = noise_->fbm(col * 0.1 + time * 0.3, row * 0.1, 3);
                double ny = noise_->fbm(col * 0.1, row * 0.1 + time * 0.3, 3);
                cx += nx * 0.12;
                cy += ny * 0.12;

                coords[idx * 2] = static_cast<float>(cx);
                coords[idx * 2 + 1] = static_cast<float>(cy);
                idx++;
            }
        }

        return EncodedFrame(0, std::move(coords), count, static_cast<int>(frameData.size()));
    }

    std::vector<uint8_t> CoordinateEncoder::decodeParticles(
        const std::vector<std::pair<double, double> > &particles, int expectedFrameLength) {
        if (expectedFrameLength < 0 || expectedFrameLength > (grid_.totalCells() + 7) / 8) {
            throw std::invalid_argument(
                "CoordinateEncoder::decodeParticles: expectedFrameLength must be in [0, " +
                std::to_string((grid_.totalCells() + 7) / 8) + "], got " +
                std::to_string(expectedFrameLength));
        }
        // Write recovered bits straight into bytes (N bits -> N/8 bytes),
        // avoiding a full per-bit array.
        int byteCount = (grid_.totalCells() + 7) / 8;
        std::vector<uint8_t> allBytes(byteCount, 0);
        for (auto [x, y]: particles) {
            auto [col, row] = grid_.centerToGrid(x, y);
            int shuffledIdx = grid_.gridToBitIndex(col, row);
            if (shuffledIdx >= 0 && shuffledIdx < grid_.totalCells()) {
                int bitIdx = invPerm_[shuffledIdx];
                allBytes[bitIdx / 8] |= static_cast<uint8_t>(1 << (7 - (bitIdx % 8)));
            }
        }

        std::vector<uint8_t> result(expectedFrameLength, 0);
        int copyLen = std::min(expectedFrameLength, static_cast<int>(allBytes.size()));
        std::copy(allBytes.begin(), allBytes.begin() + copyLen, result.begin());
        return result;
    }

    std::optional<std::vector<uint8_t> > CoordinateEncoder::tryDecodeFrame(
        const std::vector<std::pair<double, double> > &particles) {
        ErrorInfo ignored;
        return tryDecodeFrameEx(particles, ignored);
    }

    std::optional<std::vector<uint8_t> > CoordinateEncoder::tryDecodeFrameEx(
        const std::vector<std::pair<double, double> > &particles, ErrorInfo &outError) {
        outError = ErrorInfo{};
        if (particles.empty()) {
            outError = makeError(ErrorCode::NoParticles, "no particles supplied for decode");
            return std::nullopt;
        }

        int byteCount = (grid_.totalCells() + 7) / 8;
        std::vector<uint8_t> allBytes(byteCount, 0);
        for (auto [x, y]: particles) {
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

    int CoordinateEncoder::getSeqFromParticles(const std::vector<std::pair<double, double> > &particles) {
        auto frame = tryDecodeFrame(particles);
        if (!frame) return -1;
        return (frame.value()[4] << 8) | frame.value()[5];
    }

    std::optional<std::vector<uint8_t> > CoordinateEncoder::extractPayloadFromParticles(
        const std::vector<std::pair<double, double> > &particles) {
        auto frame = tryDecodeFrame(particles);
        if (!frame) return std::nullopt;
        auto header = FrameHeader::tryParse(frame.value());
        if (!header) return std::nullopt;
        return std::vector<uint8_t>(
            frame.value().begin() + FrameHeader::totalSize,
            frame.value().begin() + FrameHeader::totalSize + header->payloadLength);
    }
} // namespace particle_codec
