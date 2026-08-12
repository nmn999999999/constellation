#include "particle_codec/codec.h"
#include <algorithm>
#include <iostream>
#include <cassert>
#include <string>

using namespace particle_codec;

void testBasicRoundtrip() {
    std::cout << "  basic text encode/decode... ";
    ParticleCodec codec;
    std::string text = "Hello World";
    auto data = std::vector<uint8_t>(text.begin(), text.end());
    auto frames = codec.encode(data);
    assert(!frames.empty());

    auto &frame = frames[0];
    std::vector<std::pair<double, double> > centroids;
    for (int i = 0; i < frame.particleCount; i++)
        centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);

    auto decoded = codec.decodeCentroids(centroids);
    assert(decoded.has_value());

    auto header = FrameHeader::tryParse(decoded.value());
    assert(header.has_value());
    assert(header->payloadLength == static_cast<int>(data.size()));

    std::string result(decoded->begin() + FrameHeader::totalSize,
                       decoded->begin() + FrameHeader::totalSize + header->payloadLength);
    assert(result == "Hello World");
    std::cout << "PASS\n";
}

void testEmptyData() {
    std::cout << "  empty data... ";
    ParticleCodec codec;
    auto data = std::vector<uint8_t>{};
    auto frames = codec.encode(data);
    assert(frames.size() == 1);

    std::vector<std::pair<double, double> > centroids;
    for (int i = 0; i < frames[0].particleCount; i++)
        centroids.emplace_back(frames[0].particles[i * 2], frames[0].particles[i * 2 + 1]);

    auto decoded = codec.decodeCentroids(centroids);
    assert(decoded.has_value());
    auto header = FrameHeader::tryParse(decoded.value());
    assert(header.has_value());
    assert(header->payloadLength == 0);
    std::cout << "PASS\n";
}

void testMultiFrameData() {
    std::cout << "  multi-frame data... ";
    ParticleCodec codec;
    std::vector<uint8_t> data(1000);
    for (int i = 0; i < 1000; i++) data[i] = static_cast<uint8_t>(i % 256);

    auto frames = codec.encode(data);
    assert(frames.size() > 1);

    FrameAssembler assembler;
    for (const auto &frame: frames) {
        std::vector<std::pair<double, double> > centroids;
        for (int i = 0; i < frame.particleCount; i++)
            centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);

        auto decoded = codec.decodeCentroids(centroids);
        assert(decoded.has_value());

        auto header = FrameHeader::tryParse(decoded.value());
        assert(header.has_value());

        std::vector<uint8_t> payload(
            decoded->begin() + FrameHeader::totalSize,
            decoded->begin() + FrameHeader::totalSize + header->payloadLength);
        assembler.addFrame(header->seq, payload);
    }

    auto result = assembler.extractData();
    assert(result.has_value());
    assert(result->size() == data.size());
    assert(memcmp(result->data(), data.data(), data.size()) == 0);
    std::cout << "PASS\n";
}

void testVerifyRoundtripUtil() {
    std::cout << "  verifyRoundtrip utility... ";
    std::string text = "Test verifyRoundtrip";
    auto data = std::vector<uint8_t>(text.begin(), text.end());
    assert(ParticleCodec::verifyRoundtrip(data));
    std::cout << "PASS\n";
}

void testFixedSeed() {
    std::cout << "  no username: all instances share the same mapping... ";
    std::string text = "Same Data";
    auto data = std::vector<uint8_t>(text.begin(), text.end());

    ParticleCodec codec1;
    ParticleCodec codec2;
    auto frames1 = codec1.encode(data);
    auto frames2 = codec2.encode(data);

    const auto &f1 = frames1[0];
    const auto &f2 = frames2[0];

    bool same = true;
    int n = std::min(f1.particleCount, f2.particleCount);
    for (int i = 0; i < n; i++) {
        if (f1.particles[i * 2] != f2.particles[i * 2] ||
            f1.particles[i * 2 + 1] != f2.particles[i * 2 + 1]) {
            same = false;
            break;
        }
    }
    assert(same);
    std::cout << "PASS\n";
}

void testFrameBuilder() {
    std::cout << "  FrameBuilder build and verify CRC... ";
    std::vector<uint8_t> payload{1, 2, 3, 4, 5};
    auto frame = FrameBuilder::build(0, payload);
    assert(frame.size() == FrameHeader::totalSize + payload.size());
    assert(FrameBuilder::verifyCrc(frame));
    std::cout << "PASS\n";

    std::cout << "  CRC fails on corrupted data... ";
    frame[FrameHeader::totalSize] ^= 0xFF;
    assert(!FrameBuilder::verifyCrc(frame));
    std::cout << "PASS\n";
}

void testHamming() {
    std::cout << "  Hamming encode/decode roundtrip... ";
    std::vector<uint8_t> data{0xAB, 0xCD};
    auto encoded = HammingEncoder::encode(data);
    auto decoded = HammingEncoder::decode(encoded, static_cast<int>(data.size()));
    assert(decoded == data);
    std::cout << "PASS\n";

    std::cout << "  Hamming single bit error correction... ";
    std::vector<uint8_t> singleByte{0xFF};
    encoded = HammingEncoder::encode(singleByte);
    encoded[0] ^= 1;
    decoded = HammingEncoder::decode(encoded, 1);
    assert(decoded == singleByte);
    std::cout << "PASS\n";
}

void testPseudoRandom() {
    std::cout << "  deterministic from seed... ";
    auto seed1 = PseudoRandom::deriveSeed("input_a", "particle_codec");
    auto seed2 = PseudoRandom::deriveSeed("input_a", "particle_codec");
    PseudoRandom r1(seed1), r2(seed2);
    for (int i = 0; i < 100; i++)
        assert(r1.nextInt(1000) == r2.nextInt(1000));
    std::cout << "PASS\n";

    std::cout << "  different seeds produce different sequences... ";
    auto s1 = PseudoRandom::deriveSeed("input_a", "particle_codec");
    auto s2 = PseudoRandom::deriveSeed("input_b", "particle_codec");
    PseudoRandom rng1(s1), rng2(s2);
    int same = 0;
    for (int i = 0; i < 100; i++)
        if (rng1.nextInt(1000) == rng2.nextInt(1000)) same++;
    assert(same < 20);
    std::cout << "PASS\n";

    std::cout << "  permutation is valid... ";
    PseudoRandom rng(PseudoRandom::deriveSeed("test"));
    auto perm = rng.permutation(100);
    auto sorted = perm;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 100; i++) assert(sorted[i] == i);
    std::cout << "PASS\n";
}

void testGridMapping() {
    std::cout << "  bit index roundtrip... ";
    GridMapping grid(60, 60);
    for (int i = 0; i < grid.totalCells(); i++) {
        auto [col, row] = grid.bitIndexToGrid(i);
        int idx = grid.gridToBitIndex(col, row);
        assert(idx == i);
    }
    std::cout << "PASS\n";

    std::cout << "  bytes to bits to bytes... ";
    std::vector<uint8_t> orig{0xFF, 0x00, 0xAA, 0x55};
    auto bits = GridMapping::bytesToBits(orig);
    auto restored = GridMapping::bitsToBytes(bits);
    assert(restored == orig);
    std::cout << "PASS\n";
}

void testEccRoundtrip() {
    std::cout << "  ECC encode/decode roundtrip... ";
    ParticleCodec codec;
    std::string text = "Hello ECC!";
    auto data = std::vector<uint8_t>(text.begin(), text.end());
    auto frames = codec.encodeWithEcc(data);
    assert(!frames.empty());

    auto &frame = frames[0];
    std::vector<std::pair<double, double> > centroids;
    for (int i = 0; i < frame.particleCount; i++)
        centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);

    auto decoded = codec.decodeCentroidsWithEcc(centroids);
    assert(decoded.has_value());
    assert(decoded->size() == data.size());
    assert(memcmp(decoded->data(), data.data(), data.size()) == 0);
    std::cout << "PASS\n";
}

int main() {
    std::cout << "=== Particle Codec C++ Test Suite ===\n\n";

    std::cout << "ParticleCodec Roundtrip:\n";
    testBasicRoundtrip();
    testEmptyData();
    testMultiFrameData();
    testVerifyRoundtripUtil();
    testFixedSeed();

    std::cout << "FrameBuilder:\n";
    testFrameBuilder();

    std::cout << "HammingEncoder:\n";
    testHamming();

    std::cout << "PseudoRandom:\n";
    testPseudoRandom();

    std::cout << "GridMapping:\n";
    testGridMapping();

    std::cout << "Error Correction:\n";
    testEccRoundtrip();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
