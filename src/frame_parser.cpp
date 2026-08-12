#include "particle_codec/frame_parser.h"
#include <algorithm>
#include <unordered_map>

namespace particle_codec {
    // FrameParser
    std::optional<ParsedFrame> FrameParser::parse(const std::vector<uint8_t> &frameBytes) {
        if (frameBytes.size() < FrameHeader::totalSize) return std::nullopt;
        if (frameBytes[0] != 0xAA || frameBytes[1] != 0x55 ||
            frameBytes[2] != 0xAA || frameBytes[3] != 0x55)
            return std::nullopt;

        auto header = FrameHeader::tryParse(frameBytes);
        if (!header) return std::nullopt;

        int totalLen = FrameHeader::totalSize + header->payloadLength;
        if (static_cast<int>(frameBytes.size()) < totalLen) return std::nullopt;

        std::vector<uint8_t> payload(frameBytes.begin() + FrameHeader::totalSize,
                                     frameBytes.begin() + totalLen);

        std::vector<uint8_t> frame(frameBytes.begin(), frameBytes.begin() + totalLen);
        return ParsedFrame{header->seq, std::move(payload), FrameBuilder::verifyCrc(frame)};
    }

    std::vector<ParsedFrame> FrameParser::parseStream(const std::vector<uint8_t> &data) {
        std::vector<ParsedFrame> frames;
        size_t offset = 0;

        while (offset + FrameHeader::totalSize <= data.size()) {
            if (data[offset] != 0xAA || data[offset + 1] != 0x55 ||
                data[offset + 2] != 0xAA || data[offset + 3] != 0x55) {
                offset++;
                continue;
            }

            // Parse the header from a fixed 12-byte slice instead of copying
            // the whole remaining stream for every candidate offset.
            std::vector<uint8_t> headerBytes(
                data.begin() + offset, data.begin() + offset + FrameHeader::totalSize);
            auto header = FrameHeader::tryParse(headerBytes);
            if (!header) {
                offset++;
                continue;
            }

            int totalLen = FrameHeader::totalSize + header->payloadLength;
            if (offset + totalLen > data.size()) break;

            std::vector<uint8_t> frameBytes(data.begin() + offset, data.begin() + offset + totalLen);
            bool crcValid = FrameBuilder::verifyCrc(frameBytes);

            frames.push_back(ParsedFrame{
                header->seq,
                std::vector<uint8_t>(frameBytes.begin() + FrameHeader::totalSize, frameBytes.end()),
                crcValid
            });
            offset += totalLen;
        }
        return frames;
    }

    // FrameSplitter
    FrameSplitter::FrameSplitter(int maxPayloadBytes)
        : maxPayloadBytes_(maxPayloadBytes > 0 ? maxPayloadBytes : FrameBuilder::maxPayloadBytes()) {
    }

    std::vector<std::vector<uint8_t> > FrameSplitter::split(const std::vector<uint8_t> &data) {
        if (data.empty()) return {FrameBuilder::build(0, {})};

        std::vector<std::vector<uint8_t> > frames;
        for (int offset = 0; offset < static_cast<int>(data.size()); offset += maxPayloadBytes_) {
            int end = std::min(offset + maxPayloadBytes_, static_cast<int>(data.size()));
            std::vector<uint8_t> chunk(data.begin() + offset, data.begin() + end);
            frames.push_back(FrameBuilder::build(static_cast<int>(frames.size()), chunk));
        }
        return frames;
    }

    std::vector<std::vector<uint8_t> >
    FrameSplitter::splitWithSequence(const std::vector<uint8_t> &data, int startSeq) {
        std::vector<std::vector<uint8_t> > frames;
        int seq = startSeq;
        for (int offset = 0; offset < static_cast<int>(data.size()); offset += maxPayloadBytes_) {
            int end = std::min(offset + maxPayloadBytes_, static_cast<int>(data.size()));
            std::vector<uint8_t> chunk(data.begin() + offset, data.begin() + end);
            frames.push_back(FrameBuilder::build(seq, chunk));
            seq++;
        }
        if (frames.empty()) frames.push_back(FrameBuilder::build(seq, {}));
        return frames;
    }

    int FrameSplitter::calculateFrameCount(int dataLength) const {
        if (dataLength == 0) return 1;
        return (dataLength + maxPayloadBytes_ - 1) / maxPayloadBytes_;
    }

    // FrameAssembler
    FrameAssembler::FrameAssembler(int maxBufferSize) : maxBufferSize_(maxBufferSize) {
    }

    bool FrameAssembler::addFrame(int seq, const std::vector<uint8_t> &payload) {
        if (buffer_.count(seq)) return false;
        if (static_cast<int>(buffer_.size()) >= maxBufferSize_) return false;
        buffer_[seq] = payload;
        if (!started_ || seq < nextSeq_) {
            nextSeq_ = seq;
            started_ = true;
        }
        return true;
    }

    bool FrameAssembler::hasCompleteData() const {
        return !buffer_.empty() && buffer_.count(nextSeq_);
    }

    bool FrameAssembler::hasGap() const {
        if (buffer_.empty()) return false;
        int mn = minSeq(), mx = maxSeq();
        return (mx - mn + 1) != static_cast<int>(buffer_.size());
    }

    int FrameAssembler::minSeq() const {
        if (buffer_.empty()) return 0;
        auto it = std::min_element(buffer_.begin(), buffer_.end(),
                                   [](const auto &a, const auto &b) { return a.first < b.first; });
        return it->first;
    }

    int FrameAssembler::maxSeq() const {
        if (buffer_.empty()) return 0;
        auto it = std::max_element(buffer_.begin(), buffer_.end(),
                                   [](const auto &a, const auto &b) { return a.first < b.first; });
        return it->first;
    }

    std::optional<std::vector<uint8_t> > FrameAssembler::extractData() {
        if (buffer_.empty()) return std::nullopt;

        std::vector<std::vector<uint8_t> > chunks;
        int seq = nextSeq_;

        while (buffer_.count(seq)) {
            chunks.push_back(std::move(buffer_[seq]));
            buffer_.erase(seq);
            seq++;
        }

        if (chunks.empty()) return std::nullopt;

        size_t totalLen = 0;
        for (const auto &c: chunks) totalLen += c.size();

        std::vector<uint8_t> result(totalLen);
        size_t offset = 0;
        for (const auto &c: chunks) {
            std::copy(c.begin(), c.end(), result.begin() + offset);
            offset += c.size();
        }

        nextSeq_ = seq;
        return result;
    }

    std::optional<std::vector<uint8_t> > FrameAssembler::extractAll() {
        if (buffer_.empty()) return std::nullopt;

        std::vector<int> keys;
        for (const auto &[k, v]: buffer_) keys.push_back(k);
        std::sort(keys.begin(), keys.end());

        std::vector<std::vector<uint8_t> > chunks;
        for (int k: keys) chunks.push_back(std::move(buffer_[k]));
        buffer_.clear();

        size_t totalLen = 0;
        for (const auto &c: chunks) totalLen += c.size();

        std::vector<uint8_t> result(totalLen);
        size_t offset = 0;
        for (const auto &c: chunks) {
            std::copy(c.begin(), c.end(), result.begin() + offset);
            offset += c.size();
        }

        nextSeq_ = keys.back() + 1;
        return result;
    }

    void FrameAssembler::reset() {
        buffer_.clear();
        nextSeq_ = 0;
        started_ = false;
    }
} // namespace particle_codec
