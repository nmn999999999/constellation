#include "particle_codec/codec.h"
#include "particle_codec/error.h"
#include "particle_codec/frame_parser.h"
#include "particle_codec/grid_mapping.h"
#include "particle_codec/hamming.h"
#include "particle_codec/mapping_restorer.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace particle_codec;

static int failures = 0;

// Expects fn to throw a std::exception; prints the message as feedback.
template <typename Fn>
static void expect_throw(const char *what, Fn &&fn) {
    try {
        fn();
        std::cout << "  FAIL: " << what << " did not throw\n";
        failures++;
    } catch (const std::exception &e) {
        std::cout << "  PASS: " << what << " -> " << e.what() << "\n";
    }
}

static std::vector<std::pair<double, double> > encode_centroids(
    ParticleCodec &codec, const std::string &text) {
    auto data = std::vector<uint8_t>(text.begin(), text.end());
    auto frames = codec.encode(data);
    auto &frame = frames[0];
    std::vector<std::pair<double, double> > centroids;
    for (int i = 0; i < frame.particleCount; i++)
        centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);
    return centroids;
}

void testResultBasics() {
    std::cout << "  Result<T> ok/fail semantics... ";
    auto ok = Result<int>::success(42);
    assert(ok.ok() && ok.value() == 42);
    auto fail = Result<int>::failure(makeError(ErrorCode::CrcMismatch, "boom"));
    assert(!fail.ok() && fail.error().code == ErrorCode::CrcMismatch);
    assert(fail.error().message == "boom");
    auto vok = Result<void>::success();
    assert(vok.ok());
    auto vfail = Result<void>::failure(makeError(ErrorCode::NoParticles, "none"));
    assert(!vfail.ok() && vfail.error().code == ErrorCode::NoParticles);
    std::cout << "PASS\n";
}

void testConstructorValidation() {
    std::cout << "  invalid grids rejected:\n";
    expect_throw("ParticleCodec(0, 60)", [] { ParticleCodec c("d", 0, 60); });
    expect_throw("ParticleCodec(60, 0)", [] { ParticleCodec c("d", 60, 0); });
    expect_throw("ParticleCodec(1, 1)", [] { ParticleCodec c("d", 1, 1); });
    expect_throw("GridMapping(0, 60)", [] { GridMapping g(0, 60); });
    expect_throw("FrameAssembler(0)", [] { FrameAssembler a(0); });
    expect_throw("FrameAssembler(-1)", [] { FrameAssembler a(-1); });
}

void testFrameBuilderGuards() {
    std::cout << "  oversized payload rejected:\n";
    expect_throw("build(0, 65536 bytes)", [] {
        std::vector<uint8_t> big(65536, 1);
        FrameBuilder::build(0, big);
    });
    expect_throw("build(-1, {})", [] { FrameBuilder::build(-1, {}); });
    expect_throw("build(65536, {})", [] { FrameBuilder::build(65536, {}); });
}

void testDecodeErrorReasons() {
    std::cout << "  decode error reasons:\n";
    ParticleCodec codec;

    auto empty = codec.decodeCentroidsDetailed({});
    assert(!empty.ok());
    assert(empty.error().code == ErrorCode::NoParticles);
    assert(!codec.lastError().ok());

    auto centroids = encode_centroids(codec, "error feedback test");
    auto okRes = codec.decodeCentroidsDetailed(centroids);
    assert(okRes.ok());
    assert(codec.lastError().ok());

    // Corrupt: shift every particle so recovered bits differ.
    std::vector<std::pair<double, double> > shifted = centroids;
    for (auto &p: shifted) {
        p.first += 1.0;
        p.second += 1.0;
    }
    auto bad = codec.decodeCentroidsDetailed(shifted);
    assert(!bad.ok());
    assert(bad.error().code == ErrorCode::SyncNotFound ||
           bad.error().code == ErrorCode::CrcMismatch);
    assert(!bad.error().message.empty());
    assert(!codec.lastError().ok());
    std::cout << "  failure stage: " << errorName(bad.error().code)
              << " -> " << bad.error().message << "\n";

    // Legacy optional API records the same reason.
    auto legacy = codec.decodeCentroids(shifted);
    assert(!legacy.has_value());
    assert(codec.lastError().code == bad.error().code);
}

void testAssemblerFeedback() {
    std::cout << "  assembler error feedback:\n";
    FrameAssembler a(1);
    auto r0 = a.addFrameEx(0, {1, 2, 3});
    assert(r0.ok());

    auto dup = a.addFrameEx(0, {9});
    assert(!dup.ok() && dup.error().code == ErrorCode::FrameDuplicate);
    std::cout << "  " << dup.error().message << "\n";

    auto overflow = a.addFrameEx(1, {4});
    assert(!overflow.ok() && overflow.error().code == ErrorCode::BufferOverflow);
    std::cout << "  " << overflow.error().message << "\n";

    assert(!a.addFrame(1, {4})); // legacy bool path agrees
}

void testFeedFrameFeedback() {
    std::cout << "  feedFrameDetailed feedback:\n";
    ParticleCodec codec;
    auto centroids = encode_centroids(codec, "feed test");

    auto good = codec.feedFrameDetailed(centroids);
    assert(good.ok() && codec.lastError().ok());

    auto bad = codec.feedFrameDetailed({});
    assert(!bad.ok() && bad.error().code == ErrorCode::NoParticles);

    auto dup = codec.feedFrameDetailed(centroids);
    assert(!dup.ok() && dup.error().code == ErrorCode::FrameDuplicate);
}

void testHammingGuards() {
    std::cout << "  hamming chunk guards:\n";
    expect_throw("encodeChunk({})", [] { HammingEncoder::encodeChunk({}); });
    expect_throw("decodeChunk({})", [] { HammingEncoder::decodeChunk({}); });
    expect_throw("hasError({})", [] { HammingEncoder::hasError({}); });
    expect_throw("decode(data, -1)", [] { HammingEncoder::decode({1, 2}, -1); });
}

void testGridMappingGuards() {
    std::cout << "  grid mapping guards:\n";
    expect_throw("bytesToInt offset oob", [] { GridMapping::bytesToInt({1, 2, 3}, 5); });
    expect_throw("bytesToInt negative offset", [] { GridMapping::bytesToInt({1, 2, 3}, -1); });
    expect_throw("bytesToInt length oob", [] { GridMapping::bytesToInt({1, 2, 3}, 0, 4); });
    expect_throw("intToBytes byteCount 5", [] { GridMapping::intToBytes(1, 5); });

    GridMapping g(60, 60);
    auto bits = g.gridsToBits({{-5, 0}, {100, 100}}, 8);
    assert(bits.size() == 8 &&
           std::all_of(bits.begin(), bits.end(), [](uint8_t b) { return b == 0; }));
    std::cout << "  out-of-range grids ignored safely: PASS\n";
}

void testStreamGuards() {
    std::cout << "  float32 / decodeParticles guards:\n";
    std::vector<uint8_t> seed(32, 1);
    MappingRestorer restorer(seed, 60, 60);
    expect_throw("restoreFrameFromFloat32 count -1", [&] {
        restorer.restoreFrameFromFloat32({0.f, 0.f}, -1);
    });
    expect_throw("restoreFrameFromFloat32 count too large", [&] {
        restorer.restoreFrameFromFloat32({0.f, 0.f}, 10);
    });

    CoordinateEncoder enc(seed, 60, 60);
    expect_throw("decodeParticles len -1", [&] { enc.decodeParticles({}, -1); });
    expect_throw("decodeParticles len too big", [&] { enc.decodeParticles({}, 100000); });
}

int main() {
    std::cout << "=== Error Safety & Feedback Tests ===\n\n";

    std::cout << "Core Result API:\n";
    testResultBasics();

    std::cout << "Parameter validation:\n";
    testConstructorValidation();
    testFrameBuilderGuards();
    testHammingGuards();
    testGridMappingGuards();
    testStreamGuards();

    std::cout << "Error feedback:\n";
    testDecodeErrorReasons();
    testAssemblerFeedback();
    testFeedFrameFeedback();

    if (failures > 0) {
        std::cout << "\n" << failures << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "\nAll error-safety tests passed!\n";
    return 0;
}
