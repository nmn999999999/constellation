#include "particle_codec/codec.h"
#include "particle_codec/grid_calibrator.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <map>
#include <string>
#include <vector>

using namespace particle_codec;

static int failures = 0;

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

// Apply rotation (deg), scale and translation to canonical grid centroids,
// producing "photographed" pixel coordinates; optionally add jitter (px).
static std::vector<std::pair<double, double> > transform_centroids(
    const std::vector<std::pair<double, double> > &centroids,
    double angleDeg, double scale, double tx, double ty, double jitterPx = 0.0) {
    double rad = angleDeg * 3.14159265358979323846 / 180.0;
    double cs = std::cos(rad), sn = std::sin(rad);
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jit(-jitterPx, jitterPx);

    std::vector<std::pair<double, double> > out;
    out.reserve(centroids.size());
    for (const auto &p: centroids) {
        double x = scale * (cs * p.first - sn * p.second) + tx;
        double y = scale * (sn * p.first + cs * p.second) + ty;
        out.emplace_back(x + (jitterPx > 0 ? jit(rng) : 0.0),
                         y + (jitterPx > 0 ? jit(rng) : 0.0));
    }
    return out;
}

// Roundtrip: transform -> calibrate -> decode, verify payload.
static void check_roundtrip(const char *what,
                            const std::vector<std::pair<double, double> > &shot,
                            const std::string &message,
                            double expectAngleDeg, double expectScale,
                            ParticleCodec &codec) {
    GridCalibrator::Affine t;
    auto mapped = GridCalibrator::calibrateAndMap(shot, 60, 60, &t);
    if (!t.valid || mapped.empty()) {
        std::cout << "  FAIL: " << what << " -> calibration invalid\n";
        failures++;
        return;
    }

    // The lattice orientation is ambiguous up to 90-degree multiples; try all
    // four variants and keep the one that decodes.
    GridCalibrator::Affine variants[4] = {t, t.rotated(),
                                          t.rotated().rotated(),
                                          t.rotated().rotated().rotated()};
    for (int v = 0; v < 4; v++) {
        std::vector<std::pair<double, double> > mapped;
        mapped.reserve(shot.size());
        for (const auto &p: shot)
            mapped.push_back(variants[v].map(p.first, p.second));

        // Each variant inherits the translation of the base map rotated by
        // 90 degrees; re-align it so the mapped field sits in [0, gridSize).
        double minX = 1e18, minY = 1e18;
        for (const auto &p: mapped) {
            minX = std::min(minX, p.first);
            minY = std::min(minY, p.second);
        }
        double sx = std::round(minX - 0.5), sy = std::round(minY - 0.5);
        for (auto &p: mapped) {
            p.first -= sx;
            p.second -= sy;
        }

        auto decoded = codec.decodeCentroidsDetailed(mapped);
        if (!decoded.ok()) continue;
        auto header = FrameHeader::tryParse(decoded.value());
        std::string payload(
            decoded.value().begin() + FrameHeader::totalSize,
            decoded.value().begin() + FrameHeader::totalSize + header->payloadLength);
        if (payload != message) continue;

        // The calibrated map goes from image space back to the canonical grid,
        // so its rotation/scale are the inverse of the applied transform.
        double det = variants[v].a * variants[v].e - variants[v].b * variants[v].d;
        double scaleEst = 1.0 / std::sqrt(std::abs(det));
        double angleEst = std::atan2(variants[v].d, variants[v].a) * 180.0
                          / 3.14159265358979323846;
        double angleErr = std::min(std::abs(angleEst - expectAngleDeg),
                                   std::abs(angleEst + expectAngleDeg));
        if (angleErr > 180) angleErr = 360 - angleErr;
        double scaleErr = std::abs(scaleEst - expectScale);
        std::cout << "  PASS: " << what << " (variant " << v << ", angle err "
                  << angleErr << " deg, scale err " << scaleErr << ")\n";
        return;
    }
    std::cout << "  FAIL: " << what << " -> no orientation variant decoded\n";
    failures++;
}

int main() {
    std::cout << "=== Grid Calibrator Tests ===\n\n";

    // 425 bytes -> ~1700 particles (dense field); sparse fields are covered
    // by the video-frame tests.
    const std::string message =
        std::string(400, 'A') + "grid calibration roundtrip";
    ParticleCodec codec("particle_codec", 60, 60);
    auto centroids = encode_centroids(codec, message);
    std::cout << "Particles: " << centroids.size() << "\n";

    check_roundtrip("identity", transform_centroids(centroids, 0, 1.0, 0, 0),
                    message, 0, 1.0, codec);
    check_roundtrip("rotate 2 deg", transform_centroids(centroids, 2, 1.0, 0, 0),
                    message, 2, 1.0, codec);
    check_roundtrip("rotate -3 + scale 0.9 + shift",
                    transform_centroids(centroids, -3, 0.9, 20, 15),
                    message, -3, 0.9, codec);
    check_roundtrip("rotate 5 + scale 1.1 + shift + jitter 0.3px",
                    transform_centroids(centroids, 5, 1.1, -12, 8, 0.3),
                    message, 5, 1.1, codec);
    check_roundtrip("zoomed 90% centered",
                    transform_centroids(centroids, 0, 0.9, 24, 24),
                    message, 0, 0.9, codec);

    // Degenerate input: random points must not produce a valid transform.
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> dist(0, 480);
        std::vector<std::pair<double, double> > randomPts;
        for (int i = 0; i < 200; i++)
            randomPts.emplace_back(dist(rng), dist(rng));
        auto t = GridCalibrator::calibrate(randomPts, 60, 60);
        if (t.valid) {
            std::cout << "  FAIL: random points produced a valid transform\n";
            failures++;
        } else {
            std::cout << "  PASS: random points rejected\n";
        }
    }

    if (failures > 0) {
        std::cout << "\n" << failures << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "\nAll grid calibrator tests passed!\n";
    return 0;
}

