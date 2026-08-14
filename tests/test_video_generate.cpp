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
// particle; the scanner uses it to identify scannable frames.
static const int kMarkerSize = 4;
static const int kMarkerInset = 2; // centred inside the 8px margin
static const int kMarkerCountFull = 4; // all four corners on scannable frames
static const int kMarkerR = 210, kMarkerG = 50, kMarkerB = 230;

struct DrawParticle {
    double sx, sy; // screen coordinates (pixels)
    double alpha;  // 0..1 brightness (fade)
    double size = 1.0;   // glow scale multiplier (big star / small star)
    double bright = 1.0; // core brightness 0..1 (bright star / dim star)
    bool isDust = false; // decorative non-cyan speck (never detected)
};

// Decorative star dust: sprinkled at fully random positions with Perlin-
// modulated density, so the field reads as a natural, uneven star chart
// instead of a lattice. Dust is painted with non-cyan colours, so it never
// registers as a particle and decoding is unaffected.
static std::vector<DrawParticle> build_dust(const PerlinNoise &noise, double t,
                                            std::mt19937 &rng) {
    std::vector<DrawParticle> dust;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    // ~2200 candidate specks; density kept by the Perlin gate below.
    const int candidates = 2200;
    dust.reserve(candidates);
    for (int i = 0; i < candidates; i++) {
        double px = u01(rng) * kGridCols; // grid coords, fully random
        double py = u01(rng) * kGridRows;
        // Density field: speckles cluster where the noise is high.
        double dens = noise.fbm(px * 0.12 + t * 0.06, py * 0.12, 2);
        if (dens < 0.25) continue; // sparse regions
        double size = 0.5 + u01(rng) * 0.7;   // 0.5..1.2 (faint dust vs dots)
        double bright = 0.25 + u01(rng) * 0.5; // 0.25..0.75 (very dim)
        DrawParticle d;
        d.sx = px * kScale;
        d.sy = py * kScale;
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
// never merge 閳?the scanner always resolves all particles.
// Offset budget vs cell: global 鍗?.0px + local 鍗?.4px + codec jitter 鍗?.96px
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
// and dim small stars scatter elsewhere — no lattice regularity.
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
        double size = 0.6 + mag * 0.8;
        double bright = 0.55 + mag * 0.45;
        out.push_back({px * kScale, py * kScale, 1.0, size, bright});
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
        double size = 0.6 + mag * 0.8;
        double bright = 0.55 + mag * 0.45;
        auto it = posB.find(idx);
        if (it != posB.end()) {
            // Common particle: the irregular cell centre is identical in A and
            // B for the same cell, so the spot is stable; drift animates it.
            double px = std::clamp(pa.first + dx, cr.first + 0.4, cr.first + 0.6);
            double py = std::clamp(pa.second + dy, cr.second + 0.4, cr.second + 0.6);
            out.push_back({px * kScale, py * kScale, 1.0, size, bright});
        } else {
            double ang = hash_angle(idx);
            double d = driftPx * morphT;
            double bx = pa.first + std::cos(ang) * d;
            double by = pa.second + std::sin(ang) * d;
            double px = std::clamp(bx + dx, cr.first + 0.2, cr.first + 0.8);
            double py = std::clamp(by + dy, cr.second + 0.2, cr.second + 0.8);
            out.push_back({px * kScale, py * kScale, 1.0 - morphT, size, bright});
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
        double size = 0.6 + mag * 0.8;
        double bright = 0.55 + mag * 0.45;
        double ang = hash_angle(idx + 7);
        double d = driftPx * (1.0 - morphT);
        double bx = pb.first - std::cos(ang) * d;
        double by = pb.second - std::sin(ang) * d;
        double px = std::clamp(bx + dx, cr.first + 0.2, cr.first + 0.8);
        double py = std::clamp(by + dy, cr.second + 0.2, cr.second + 0.8);
        out.push_back({px * kScale, py * kScale, morphT, size, bright});
    }
    return out;
}

// ---- Pure-pixel nebula renderer (no GDI) --------------------------------
// A fully procedural star-chart look: a Perlin cloud background in deep
// blue-violet, soft radial-gradient star halos whose brightness follows the
// same nebula field (bright star clusters where the cloud is dense, dim
// scattered stars elsewhere), plus density-modulated decorative dust. Only
// the bright cyan cores satisfy is_cyan() (R<80, G>100, B>100, B>R+20,
// G>R+20), so decoding is unaffected.

static inline uint8_t mix8(int bg, int target, double a) {
    return static_cast<uint8_t>(bg + (target - bg) * a);
}

// Background: a low-frequency Perlin cloud sampled every 4 px, bilinearly
// interpolated to full resolution, so the field reads as a nebula instead of
// a flat black canvas. Colour is deep blue-violet, never cyan (G stays low).
static void paint_nebula_background(std::vector<uint8_t> &px,
                                    const PerlinNoise &noise, double t) {
    const int step = 4;
    const int gw = kExportW / step + 1, gh = kExportH / step + 1;
    std::vector<float> cloud(gw * gh);
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            double cx = (gx * step) / kScale;
            double cy = (gy * step) / kScale;
            double n1 = noise.fbm(cx * 0.045 + t * 0.008, cy * 0.045, 2);
            double n2 = noise.fbm(cx * 0.16 + 9.3, cy * 0.16 + 2.7, 2);
            cloud[gy * gw + gx] = static_cast<float>(n1 * 0.65 + n2 * 0.35);
        }
    }
    for (int y = 0; y < kExportH; y++) {
        double fy = static_cast<double>(y) / step;
        int gy0 = static_cast<int>(fy);
        double ty = fy - gy0;
        for (int x = 0; x < kExportW; x++) {
            double fx = static_cast<double>(x) / step;
            int gx0 = static_cast<int>(fx);
            double tx = fx - gx0;
            auto sample = [&](int gxi, int gyi) {
                gxi = std::clamp(gxi, 0, gw - 1);
                gyi = std::clamp(gyi, 0, gh - 1);
                return cloud[gyi * gw + gxi];
            };
            float v00 = sample(gx0, gy0), v10 = sample(gx0 + 1, gy0);
            float v01 = sample(gx0, gy0 + 1), v11 = sample(gx0 + 1, gy0 + 1);
            float v = (v00 * (1 - tx) + v10 * tx) * (1 - ty) +
                      (v01 * (1 - tx) + v11 * tx) * ty;
            double k = v * 0.5 + 0.5; // [0,1]
            int idx = (y * kExportW + x) * 3;
            px[idx] = mix8(4, 30, k);
            px[idx + 1] = mix8(4, 18, k);
            px[idx + 2] = mix8(15, 64, k);
        }
    }
}

// Soft radial-gradient halo: non-cyan blue-violet, fades into the background
// so neighbouring halos blend into a continuous nebula glow.
static void draw_halo(std::vector<uint8_t> &px, int cx, int cy,
                      double size, double bright, double alpha) {
    int gr = static_cast<int>(6.5 * size) + 2;
    for (int dy = -gr; dy <= gr; dy++) {
        for (int dx = -gr; dx <= gr; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= kExportW || y < 0 || y >= kExportH) continue;
            double d = std::sqrt(dx * dx + dy * dy);
            if (d > gr) continue;
            double fade = 1.0 - d / gr;
            double a = alpha * fade * fade;
            int idx = (y * kExportW + x) * 3;
            if (d < gr * 0.45) { // inner blue glow
                px[idx] = mix8(px[idx], 0, a);
                px[idx + 1] = mix8(px[idx + 1], 62, a);
                px[idx + 2] = mix8(px[idx + 2], 118, a);
            } else {             // outer violet haze
                px[idx] = mix8(px[idx], 34, a);
                px[idx + 1] = mix8(px[idx + 1], 20, a);
                px[idx + 2] = mix8(px[idx + 2], 74, a);
            }
        }
    }
}

// Bright cyan core (the scanner's detection target). A symmetric filled disc
// so the detected centroid lands exactly on the centre; brightness modulated
// by the nebula field (G always stays > 100 so every star is resolvable).
static void draw_core(std::vector<uint8_t> &px, int cx, int cy,
                      double bright, double alpha) {
    const int cr = 2;
    int g = mix8(0, 185, bright);
    int b = mix8(0, 212, bright);
    for (int dy = -cr; dy <= cr; dy++) {
        for (int dx = -cr; dx <= cr; dx++) {
            if (dx * dx + dy * dy > cr * cr + 1) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= kExportW || y < 0 || y >= kExportH) continue;
            int idx = (y * kExportW + x) * 3;
            px[idx] = mix8(px[idx], 0, alpha);
            px[idx + 1] = mix8(px[idx + 1], g, alpha);
            px[idx + 2] = mix8(px[idx + 2], b, alpha);
        }
    }
}

// Unified renderer: procedural nebula background, decorative dust, star
// halos+cores, corner markers, then a 24-bit BMP.
static void render_to_bmp(const std::vector<DrawParticle> &particles,
                          int markerCount, const std::string &path,
                          const PerlinNoise &noise, double t) {
    std::vector<uint8_t> px(kExportW * kExportH * 3);
    paint_nebula_background(px, noise, t);

    // Decorative dust: faint blue-grey specks, G stays far below 100.
    for (const auto &p : particles) {
        if (!p.isDust) continue;
        int sx = static_cast<int>(p.sx) + kMargin;
        int sy = static_cast<int>(p.sy) + kMargin;
        int r = 1;
        double a = p.alpha * p.bright * 0.55;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy > r * r) continue;
                int x = sx + dx, y = sy + dy;
                if (x < 0 || x >= kExportW || y < 0 || y >= kExportH) continue;
                int idx = (y * kExportW + x) * 3;
                px[idx] = mix8(px[idx], 34, a);
                px[idx + 1] = mix8(px[idx + 1], 42, a);
                px[idx + 2] = mix8(px[idx + 2], 76, a);
            }
    }
    // Star halos (data particles).
    for (const auto &p : particles) {
        if (p.isDust) continue;
        int sx = static_cast<int>(p.sx) + kMargin;
        int sy = static_cast<int>(p.sy) + kMargin;
        draw_halo(px, sx, sy, p.size, p.bright, p.alpha);
    }
    // Cores on top (never covered by halos).
    for (const auto &p : particles) {
        if (p.isDust) continue;
        int sx = static_cast<int>(p.sx) + kMargin;
        int sy = static_cast<int>(p.sy) + kMargin;
        draw_core(px, sx, sy, p.bright, p.alpha);
    }

    // Corner markers: bright purple squares inside the black margin.
    if (markerCount > 0) {
        const int corners[4][2] = {
            {kMarkerInset, kMarkerInset},                       // TL
            {kExportW - kMarkerInset - kMarkerSize, kMarkerInset}, // TR
            {kMarkerInset, kExportH - kMarkerInset - kMarkerSize}, // BL
            {kExportW - kMarkerInset - kMarkerSize, kExportH - kMarkerInset - kMarkerSize}, // BR
        };
        for (int c = 0; c < markerCount && c < 4; c++) {
            for (int dy = 0; dy < kMarkerSize; dy++)
                for (int dx = 0; dx < kMarkerSize; dx++) {
                    int x = corners[c][0] + dx, y = corners[c][1] + dy;
                    int idx = (y * kExportW + x) * 3;
                    px[idx] = kMarkerR;
                    px[idx + 1] = kMarkerG;
                    px[idx + 2] = kMarkerB;
                }
        }
    }

    // Write 24-bit BMP (pixel storage is BGR, our buffer is RGB -> swap).
    std::vector<uint8_t> bmpData(px.size());
    for (size_t i = 0; i < px.size(); i += 3) {
        bmpData[i] = px[i + 2];     // B
        bmpData[i + 1] = px[i + 1]; // G
        bmpData[i + 2] = px[i];     // R
    }
    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42;
    bfh.bfSize = 54 + static_cast<DWORD>(bmpData.size());
    bfh.bfOffBits = 54;
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(&bfh, 1, sizeof(bfh), f);
        BITMAPINFOHEADER bih = {};
        bih.biSize = sizeof(bih);
        bih.biWidth = kExportW;
        bih.biHeight = -kExportH;
        bih.biPlanes = 1;
        bih.biBitCount = 24;
        bih.biCompression = BI_RGB;
        fwrite(&bih, 1, sizeof(bih), f);
        fwrite(bmpData.data(), 1, bmpData.size(), f);
        fclose(f);
    }
}

int main(int argc, char *argv[]) {
    std::string outDir = (argc > 1) ? argv[1] : "build/video_test/anim";
    int payloadBytes = (argc > 2) ? std::atoi(argv[2]) : 1200;
    int transition = (argc > 3) ? std::atoi(argv[3]) : 0;
    bool eccMode = (argc > 4) && (std::string(argv[4]) == "ecc");

    if (payloadBytes <= 0) {
        std::cerr << "Error: payload_bytes must be positive" << std::endl;
        return 1;
    }
    if (transition < 0 || transition >= kAnimFramesPerDataFrame) {
        std::cerr << "Error: transition_frames must be in [0, "
                  << kAnimFramesPerDataFrame - 1 << "]" << std::endl;
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

    // Keep the same seed/domain the decoder will use.
    std::string domain = "particle_codec";
    auto seed = PseudoRandom::deriveSeed("demo_user", domain);
    CoordinateEncoder encoder(seed, kGridCols, kGridRows);

    int chunkSize = encoder.maxPayloadBytes();
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
            render_to_bmp(withDust(build_frame(frame, t, driftNoise), t),
                          kMarkerCountFull, outDir + "/" + name, driftNoise, t);

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
                render_to_bmp(withDust(build_morph(frameA, frameB, t, morphT, driftNoise), t), 0,
                              outDir + "/" + name, driftNoise, t);

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


