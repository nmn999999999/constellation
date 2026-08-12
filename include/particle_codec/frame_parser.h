#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include "error.h"
#include "frame_builder.h"
#include <unordered_map>

namespace particle_codec {
    struct ParsedFrame {
        int seq;
        std::vector<uint8_t> payload;
        bool crcValid;
    };

    class FrameParser {
    public:
        std::optional<ParsedFrame> parse(const std::vector<uint8_t> &frameBytes);

        std::vector<ParsedFrame> parseStream(const std::vector<uint8_t> &data);

        void reset() {
        }
    };

    class FrameSplitter {
    public:
        explicit FrameSplitter(int maxPayloadBytes = -1);

        std::vector<std::vector<uint8_t> > split(const std::vector<uint8_t> &data);

        std::vector<std::vector<uint8_t> > splitWithSequence(const std::vector<uint8_t> &data, int startSeq = 0);

        int calculateFrameCount(int dataLength) const;

    private:
        int maxPayloadBytes_;
    };

    class FrameAssembler {
    public:
        // Throws std::invalid_argument unless maxBufferSize >= 1.
        explicit FrameAssembler(int maxBufferSize = 256);

        // Convenience wrapper; use addFrameEx for failure reasons.
        bool addFrame(int seq, const std::vector<uint8_t> &payload);

        // Error-safe variant: reports FrameDuplicate / BufferOverflow.
        Result<void> addFrameEx(int seq, const std::vector<uint8_t> &payload);

        bool hasCompleteData() const;

        bool hasGap() const;

        std::optional<std::vector<uint8_t> > extractData();

        std::optional<std::vector<uint8_t> > extractAll();

        void reset();

        int bufferedCount() const { return static_cast<int>(buffer_.size()); }
        int nextExpectedSeq() const { return nextSeq_; }

        int minSeq() const;

        int maxSeq() const;

    private:
        int maxBufferSize_;
        std::unordered_map<int, std::vector<uint8_t> > buffer_;
        int nextSeq_ = 0;
        bool started_ = false;
    };
} // namespace particle_codec
