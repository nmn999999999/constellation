// Multi-frame animation generator: encodes a large payload into multiple data
// frames, renders each one at several animation times (Perlin drift), and
// writes an export-style BMP sequence suitable for ffmpeg video composition.
//
// Animation model (v2, no pauses):
//   * Each data frame renders `pure` frames with 4 corner markers = the frames
//     the scanner should decode ("scannable frames").
//   * Between consecutive data frames, a particle-morph transition flows the
//     particles from layout A to layout B: cells present in both frames keep a
//     particle that eases to its new position; cells leaving A fade out and
//     drift away; cells entering B fade in and drift in. There is no crossfade
//     and no hard cut, so the animation is continuous and pause-free.
//   * Transition frames carry no corner markers, so the scanner automatically
//     skips them and grabs the next scannable frame.
//
// Usage: test_video_generate.exe <output_dir> [payload_bytes] [transition_frames] [ecc]
//   transition_frames > 0 enables the morph between consecutive data frames.
//   "ecc" enables Hamming (7,4) error correction on the payload.
#include "particle_codec/codec.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unordered_map>
#include <random>

#define NOMINMAX
#include <windows.h>
#include "../demo/nebula_render.h"

using namespace particle_codec;

static const int kGridCols = 60, kGridRows = 60;
// A black margin around the grid carries the corner markers, so markers never
// overlap the particles of the outermost cells (particle cores live inside the
// 480x480 grid area, markers live in the 8px frame).
static const int kMargin = 8;          // black border for corner markers (px)
static const int kGridW = kGridCols * 8; // 480
static const int kGridH = kGridRows * 8; // 480
static const int kExportW = kGridW + kMargin * 2; // 496
static const int kExportH = kGridH + kMargin * 2; // 496
static const double kScale = 8.0;
static const double kFps = 30.0;
static const int kAnimFramesPerDataFrame = 30;

// Corner marker geometry: a 4x4 bright-purple square inside the black margin,
// one near each image corner. Purple is deliberately NOT cyan so it never
// registers as a particle; the scanner uses it to identify scannable frames.
static const int kMarkerSize = 4;
static const int kMarkerInset = 2; // centred inside the 8px margin
static const int kMarkerCountFull = 4; // all four corners on scannable frames
static const int kMarkerR = 210, kMarkerG = 50, kMarkerB = 230;

using DrawParticle = nebula::Star;

// Decorative star dust: sprinkled at fully random positions with Perlin-
// modulated density, so the field reads as a natural, uneven star chart
// instead of a lattice. Dust is painted with non-cyan colours, so it never
// registers as a particle and decoding is unaffected.
static std::vector<DrawParticle> build_dust(const PerlinNoise &noise, double t,
                                            std::mt19937 &rng) {
    std::vector<DrawParticle> dust;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    // ~4500 candidate specks; density kept by the Perlin gate below.
    const int candidates = 4500;
    dust.reserve(candidates);
    for (int i = 0; i < candidates; i++) {
        double px = u01(rng) * kGridCols; // grid coords, fully random
        double py = u01(rng) * kGridRows;
        // Density field: speckles cluster where the noise is high.
        double dens = noise.fbm(px * 0.12 + t * 0.06, py * 0.12, 2);
        if (dens < 0.25) continue; // sparse regions
        double size = 0.4 + u01(rng) * 0.9;   // 0.4..1.3 (faint dust vs dots)
        double bright = 0.2 + u01(rng) * 0.55; // 0.2..0.75 (very dim)
        DrawParticle d;
        d.gx = px;
        d.gy = py;
        d.alpha = 1.0;
        d.size = size;
        d.bright = bright;
        d.isDust = true;
        dust.push_back(d);
    }
    return dust;
}

// Deterministic per-cell pseudo-random value in [0,1]; different salt gives
// independent values (used for star position offset, size and brightness).
static double cell_random(int col, int row, int salt) {
    unsigned int h = static_cast<unsigned int>(col * 73856093u) ^
                     static_cast<unsigned int>(row * 19349663u) ^
                     static_cast<unsigned int>(salt * 83492791u);
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xFFFF) / 65535.0;
}

// Deterministic pseudo-random angle for a grid index (used for the drift
// direction of particles entering/leaving during a morph).
static double hash_angle(int idx) {
    unsigned int h = static_cast<unsigned int>(idx) * 2654435761u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return static_cast<double>(h % 3600) / 10.0 * 3.14159265358979323846 / 180.0;
}

// Apple-Watch-style particle cloud: the whole field flows as one body along a
// slow low-frequency Perlin path (like drifting nebula gas), plus a tiny
// per-particle wander for organic liveliness. Because the shared global flow
// keeps every particle's *relative* spacing unchanged, neighbouring cores can
// never merge 闂?the scanner always resolves all particles.
// Offset budget vs cell: global 闂?.0px + local 闂?.4px + codec jitter 闂?.96px
// = max ~3.36px < 4px (half cell), so floor() mapping stays correct and every
// scannable frame decodes directly (fusion only as a fallback).
static const double kLocalAmp = 0.4 / kScale;   // per-particle micro-wander (px)
static const double kGlobalAmp = 2.0 / kScale;  // shared nebula flow (px)

static std::pair<double, double> drift_offset(const PerlinNoise &noise,
                                              int col, int row,
                                              double t, int phase) {
    // Global nebula flow: identical offset for every particle, so the cloud
    // breathes and drifts as one without changing internal distances.
    double gx = noise.fbm(t * 0.05, 3.3, 2) * kGlobalAmp;
    double gy = noise.fbm(7.7, t * 0.05, 2) * kGlobalAmp;
    // Per-particle micro-wander: small, so it never merges neighbouring cores.
    double nx = noise.fbm(col * 0.04 + t * 0.12, row * 0.04 + phase * 0.17, 3);
    double ny = noise.fbm(col * 0.04 + phase * 0.17 + 7.7,
                          row * 0.04 + t * 0.12, 3);
    return {nx * kLocalAmp + gx, ny * kLocalAmp + gy};
}

// Corner markers live in the black margin (outside the grid), so particles of
// the outermost cells can drift freely without ever being hidden.

// The codec itself now places every particle at a deterministic irregular
// centre inside its cell (GridMapping::irregularCenters). This renderer just
// adds the Perlin drift (clamped inside the cell so floor() stays exact) and
// per-cell size/brightness for the star-chart look.

// Build the draw list of a pure frame: every particle sits at its irregular
// cell centre plus the continuous Perlin drift. Size and brightness follow
// the nebula field, so bright large stars cluster where the cloud is dense
// and dim small stars scatter elsewhere 闂?no lattice regularity.
static std::vector<DrawParticle> build_frame(const EncodedFrame &frame, double t,
                                             const PerlinNoise &noise) {
    std::vector<DrawParticle> out;
    out.reserve(static_cast<size_t>(frame.particleCount));
    for (int i = 0; i < frame.particleCount; i++) {
        double x = frame.particles[i * 2], y = frame.particles[i * 2 + 1];
        int col = static_cast<int>(std::floor(x));
        int row = static_cast<int>(std::floor(y));
        auto [dx, dy] = drift_offset(noise, col, row, t, i);
        double px = std::clamp(x + dx, col + 0.4, col + 0.6);
        double py = std::clamp(y + dy, row + 0.4, row + 0.6);
        // Nebula magnitude: high in star-cloud bands, low in the voids.
        double mag = 0.5 + 0.5 * noise.fbm(col * 0.05 + t * 0.02,
                                           row * 0.05, 2); // [0,1]
        // Wild size/magnitude contrast: tiny dim specks (barely a core) up to
        // bright stars with large halos. Size is capped so overlapping halos
        // never stack their green channel above the is_cyan threshold.
        double size = 0.4 + mag * mag * 1.2;   // 0.4 .. 1.6
        double bright = 0.6 + mag * 0.4;     // 0.6 .. 1.0
        out.push_back({px, py, size, bright, 1.0});
    }
    return out;
}

// Build the draw list of a morph between frame A (morphT=0) and frame B
// (morphT=1). Layout eases between the two grids while every particle keeps
// floating with the continuous Perlin drift:
//   * cells in both frames: particle eases from A to B position, alpha 1
//   * cells only in A: particle drifts away and fades out (alpha 1-morphT)
//   * cells only in B: particle drifts in and fades in (alpha morphT)
static std::vector<DrawParticle> build_morph(const EncodedFrame &frameA,
                                             const EncodedFrame &frameB,
                                             double t, double morphT,
                                             const PerlinNoise &noise) {
    auto gridIdx = [](double x, double y) {
        int col = static_cast<int>(std::floor(x));
        int row = static_cast<int>(std::floor(y));
        return row * kGridCols + col;
    };
    auto gridColRow = [](double x, double y) {
        return std::make_pair(static_cast<int>(std::floor(x)),
                              static_cast<int>(std::floor(y)));
    };
    std::unordered_map<int, std::pair<double, double> > posA, posB;
    for (int i = 0; i < frameA.particleCount; i++) {
        double x = frameA.particles[i * 2], y = frameA.particles[i * 2 + 1];
        posA[gridIdx(x, y)] = {x, y};
    }
    for (int i = 0; i < frameB.particleCount; i++) {
        double x = frameB.particles[i * 2], y = frameB.particles[i * 2 + 1];
        posB[gridIdx(x, y)] = {x, y};
    }

    const double driftPx = 4.0 / kScale; // grid units a particle drifts while fading
    std::vector<DrawParticle> out;
    out.reserve(std::max(posA.size(), posB.size()));

    // Particles present in A: common ones ease to B, A-only ones fade+drift out.
    for (const auto &kv : posA) {
        const int idx = kv.first;
        const std::pair<double, double> &pa = kv.second;
        auto cr = gridColRow(pa.first, pa.second);
        auto [dx, dy] = drift_offset(noise, cr.first, cr.second, t, idx);
        double mag = 0.5 + 0.5 * noise.fbm(cr.first * 0.05 + t * 0.02,
                                           cr.second * 0.05, 2);
        double size = 0.4 + mag * mag * 1.2;
        double bright = 0.6 + mag * 0.4;
        auto it = posB.find(idx);
        if (it != posB.end()) {
            // Common particle: the irregular cell centre is identical in A and
            // B for the same cell, so the spot is stable; drift animates it.
            double px = std::clamp(pa.first + dx, cr.first + 0.4, cr.first + 0.6);
            double py = std::clamp(pa.second + dy, cr.second + 0.4, cr.second + 0.6);
            out.push_back({px, py, size, bright, 1.0});
        } else {
            double ang = hash_angle(idx);
            double d = driftPx * morphT;
            double bx = pa.first + std::cos(ang) * d;
            double by = pa.second + std::sin(ang) * d;
            double px = std::clamp(bx + dx, cr.first + 0.2, cr.first + 0.8);
            double py = std::clamp(by + dy, cr.second + 0.2, cr.second + 0.8);
            out.push_back({px, py, size, bright, 1.0 - morphT});
        }
    }
    // Particles only in B: fade in while drifting in from the drift distance.
    for (const auto &kv : posB) {
        const int idx = kv.first;
        if (posA.count(idx)) continue;
        const std::pair<double, double> &pb = kv.second;
        auto cr = gridColRow(pb.first, pb.second);
        auto [dx, dy] = drift_offset(noise, cr.first, cr.second, t, idx);
        double mag = 0.5 + 0.5 * noise.fbm(cr.first * 0.05 + t * 0.02,
                                           cr.second * 0.05, 2);
        double size = 0.4 + mag * mag * 1.2;
        double bright = 0.6 + mag * 0.4;
        double ang = hash_angle(idx + 7);
        double d = driftPx * (1.0 - morphT);
        double bx = pb.first - std::cos(ang) * d;
        double by = pb.second - std::sin(ang) * d;
        double px = std::clamp(bx + dx, cr.first + 0.2, cr.first + 0.8);
        double py = std::clamp(by + dy, cr.second + 0.2, cr.second + 0.8);
        out.push_back({px, py, size, bright, morphT});
    }
    return out;
}

// Rendering moved to demo/nebula_render.h (shared nebula renderer).

int main(int argc, char *argv[]) {
    std::string outDir = (argc > 1) ? argv[1] : "build/video_test/anim";
    int payloadBytes = (argc > 2) ? std::atoi(argv[2]) : -1;
    int transition = (argc > 3) ? std::atoi(argv[3]) : 0;
    bool eccMode = (argc > 4) && (std::string(argv[4]) == "ecc");

    if (transition < 0 || transition >= kAnimFramesPerDataFrame) {
        std::cerr << "Error: transition_frames must be in [0, "
                  << kAnimFramesPerDataFrame - 1 << "]" << std::endl;
        return 1;
    }

    // Keep the same seed/domain the decoder will use.
    std::string domain = "particle_codec";
    auto seed = PseudoRandom::deriveSeed("demo_user", domain);
    CoordinateEncoder encoder(seed, kGridCols, kGridRows);

    int chunkSize = encoder.maxPayloadBytes();
    if (payloadBytes < 0) {
        // Default to 3 full chunks so every data frame carries a similar
        // particle count (otherwise the final short frame looks sparse).
        payloadBytes = chunkSize * 3;
        std::cout << "Default payload: " << payloadBytes << " bytes ("
                  << chunkSize << " x 3, uniform per-frame density)" << std::endl;
    }
    if (payloadBytes <= 0) {
        std::cerr << "Error: payload_bytes must be positive" << std::endl;
        return 1;
    }

    // Create the output directory if missing (CreateDirectoryA is ANSI-safe
    // for Chinese paths; fopen below silently fails on a missing directory).
    if (!outDir.empty()) {
        std::string dir = outDir;
        for (size_t i = 0; i < dir.size(); i++)
            if (dir[i] == '/') dir[i] = '\\';
        CreateDirectoryA(dir.c_str(), nullptr);
    }

    // Deterministic, recognizable payload spanning multiple data frames.
    std::vector<uint8_t> data(static_cast<size_t>(payloadBytes));
    const char *head = "Constellation multi-frame video test: ";
    size_t headLen = std::strlen(head);
    for (size_t i = 0; i < data.size(); i++) {
        if (i < headLen) {
            data[i] = static_cast<uint8_t>(head[i]);
        } else {
            data[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
        }
    }

    // Optionally apply Hamming (7,4) ECC before framing.
    std::vector<uint8_t> framedData = data;
    if (eccMode) {
        framedData = HammingEncoder::encode(data);
        std::cout << "ECC: Hamming (7,4) enabled, framed payload "
                  << framedData.size() << " bytes" << std::endl;
    }
    std::vector<std::vector<uint8_t> > frameBytesList;
    for (int offset = 0; offset < static_cast<int>(framedData.size()); offset += chunkSize) {
        int end = std::min(offset + chunkSize, static_cast<int>(framedData.size()));
        int seq = static_cast<int>(frameBytesList.size());
        frameBytesList.push_back(FrameBuilder::build(
            seq, std::vector<uint8_t>(framedData.begin() + offset,
                                      framedData.begin() + end)));
    }

    std::cout << "=== Multi-frame Video Generation ===" << std::endl;
    std::cout << "Payload: " << data.size() << " bytes -> " << frameBytesList.size()
              << " data frame(s), chunk=" << chunkSize << " bytes" << std::endl;
    if (transition > 0) {
        std::cout << "Morph transition: " << transition
                  << " frame(s) of particle easing between data frames"
                  << std::endl;
    }
    std::cout << "Scannable frames carry 4 corner markers; the scanner "
                 "auto-grabs them and skips transitions"
              << std::endl;

    // Raw payload for later verification by the decoder test.
    {
        std::string rawPath = outDir + "/../payload.bin";
        FILE *f = fopen(rawPath.c_str(), "wb");
        if (f) {
            fwrite(data.data(), 1, data.size(), f);
            fclose(f);
        }
    }

    int totalAnim = 0;
    // Dedicated Perlin field for the visible drift animation.
    auto noiseSeed = PseudoRandom::deriveSeed("particle_codec_drift", "video_gen");
    PerlinNoise driftNoise(noiseSeed);
    // Merge decorative star dust (fixed seed -> stable positions; the Perlin
    // density field still breathes with t so the dust clusters shift slowly).
    auto withDust = [&](std::vector<DrawParticle> stars, double t) {
        std::mt19937 rng(42);
        auto dust = build_dust(driftNoise, t, rng);
        stars.insert(stars.end(), dust.begin(), dust.end());
        return stars;
    };
    FILE *meta = fopen((outDir + "/anim_meta.txt").c_str(), "w");
    for (size_t d = 0; d < frameBytesList.size(); d++) {
        int pureFrames = kAnimFramesPerDataFrame - transition;
        for (int a = 0; a < pureFrames; a++) {
            double t = totalAnim / kFps; // continuous timeline across all frames
            auto frame = encoder.encodeSingleFrame(frameBytesList[d], t);

            char name[64];
            std::snprintf(name, sizeof(name), "anim_%03d.bmp", totalAnim);
            nebula::render(withDust(build_frame(frame, t, driftNoise), t),
                           kGridCols, kGridRows, kMargin, /*markers=*/true, t,
                           driftNoise, outDir + "/" + name);

            if (meta) {
                std::fprintf(meta, "%03d scannable seq=%zu particles=%d t=%.2f\n",
                             totalAnim, d, frame.particleCount, t);
            }
            totalAnim++;
        }

        // Particle morph into the next data frame (skip after the last one).
        if (d + 1 < frameBytesList.size() && transition > 0) {
            for (int tr = 0; tr < transition; tr++) {
                double t = totalAnim / kFps;
                auto frameA = encoder.encodeSingleFrame(frameBytesList[d], t);
                auto frameB = encoder.encodeSingleFrame(frameBytesList[d + 1], t);
                double morphT = static_cast<double>(tr + 1) / (transition + 1);

                char name[64];
                std::snprintf(name, sizeof(name), "anim_%03d.bmp", totalAnim);
                // Transition frames carry no markers -> scanner skips them.
                nebula::render(withDust(build_morph(frameA, frameB, t, morphT, driftNoise), t),
                               kGridCols, kGridRows, kMargin, /*markers=*/false, t,
                               driftNoise, outDir + "/" + name);

                if (meta) {
                    std::fprintf(meta, "%03d morph %zu->%zu morphT=%.3f t=%.2f\n",
                                 totalAnim, d, d + 1, morphT, t);
                }
                totalAnim++;
            }
        }
    }
    if (meta) fclose(meta);

    std::cout << "Wrote " << totalAnim << " animation frames to " << outDir << std::endl;
    std::cout << "Next: ffmpeg -framerate 30 -i " << outDir
              << "/anim_%03d.bmp -c:v libx264 -pix_fmt yuv420p -crf 23 "
              << outDir << "/../constellation.mp4" << std::endl;
    return 0;
}










