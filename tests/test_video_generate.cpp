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
};

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
// never merge 鈥?the scanner always resolves all particles.
// Offset budget vs cell: global 卤2.0px + local 卤0.4px + codec jitter 卤0.96px
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

// Star chart placement: a particle sits at a pseudo-random spot inside its own
// grid cell (not the centre), so the field looks like a natural star chart
// instead of a regular lattice. floor() decoding stays exact because the
// offset is clamped to [0.3, 0.7] inside the cell; combined with the 2px core
// (4px diameter) the nearest neighbours can never merge (min spacing
// 0.6 cell = 4.8px > 4px). Size and brightness also vary per cell for
// bright/dim, large/small stars.
static std::pair<double, double> star_spot(int col, int row) {
    double ox = 0.3 + cell_random(col, row, 1) * 0.4; // [0.3, 0.7]
    double oy = 0.3 + cell_random(col, row, 2) * 0.4;
    return {ox, oy};
}

// Build the draw list of a pure frame: every particle sits at its star-chart
// spot plus the continuous Perlin drift (full brightness).
static std::vector<DrawParticle> build_frame(const EncodedFrame &frame, double t,
                                             const PerlinNoise &noise) {
    std::vector<DrawParticle> out;
    out.reserve(static_cast<size_t>(frame.particleCount));
    for (int i = 0; i < frame.particleCount; i++) {
        double x = frame.particles[i * 2], y = frame.particles[i * 2 + 1];
        int col = static_cast<int>(std::floor(x));
        int row = static_cast<int>(std::floor(y));
        auto [ox, oy] = star_spot(col, row);
        auto [dx, dy] = drift_offset(noise, col, row, t, i);
        double px = std::clamp(col + ox + dx, col + 0.3, col + 0.7);
        double py = std::clamp(row + oy + dy, row + 0.3, row + 0.7);
        double size = 0.7 + cell_random(col, row, 3) * 0.6;
        double bright = 0.6 + cell_random(col, row, 4) * 0.4;
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

    auto spot = [](int col, int row) { return star_spot(col, row); };

    // Particles present in A: common ones ease to B, A-only ones fade+drift out.
    for (const auto &kv : posA) {
        const int idx = kv.first;
        const std::pair<double, double> &pa = kv.second;
        auto cr = gridColRow(pa.first, pa.second);
        auto [ox, oy] = star_spot(cr.first, cr.second);
        auto [dx, dy] = drift_offset(noise, cr.first, cr.second, t, idx);
        double size = 0.7 + cell_random(cr.first, cr.second, 3) * 0.6;
        double bright = 0.6 + cell_random(cr.first, cr.second, 4) * 0.4;
        auto it = posB.find(idx);
        if (it != posB.end()) {
            // Common particle: layout (star spot) of A and B is identical for
            // the same cell, so the spot is stable; drift still animates it.
            double bx = cr.first + ox;
            double by = cr.second + oy;
            double px = std::clamp(bx + dx, cr.first + 0.3, cr.first + 0.7);
            double py = std::clamp(by + dy, cr.second + 0.3, cr.second + 0.7);
            out.push_back({px * kScale, py * kScale, 1.0, size, bright});
        } else {
            double ang = hash_angle(idx);
            double d = driftPx * morphT;
            double bx = cr.first + ox + std::cos(ang) * d;
            double by = cr.second + oy + std::sin(ang) * d;
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
        auto [ox, oy] = star_spot(cr.first, cr.second);
        auto [dx, dy] = drift_offset(noise, cr.first, cr.second, t, idx);
        double size = 0.7 + cell_random(cr.first, cr.second, 3) * 0.6;
        double bright = 0.6 + cell_random(cr.first, cr.second, 4) * 0.4;
        double ang = hash_angle(idx + 7);
        double d = driftPx * (1.0 - morphT);
        double bx = cr.first + ox - std::cos(ang) * d;
        double by = cr.second + oy - std::sin(ang) * d;
        double px = std::clamp(bx + dx, cr.first + 0.2, cr.first + 0.8);
        double py = std::clamp(by + dy, cr.second + 0.2, cr.second + 0.8);
        out.push_back({px * kScale, py * kScale, morphT, size, bright});
    }
    return out;
}

// Unified renderer: draws a particle list (brightness scaled by alpha) plus
// `markerCount` corner markers, then exports a 24-bit BMP.
static void render_to_bmp(const std::vector<DrawParticle> &particles,
                          int markerCount, const std::string &path) {
    HDC hdc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, kExportW, kExportH);
    SelectObject(memDc, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(10, 10, 26));
    RECT fr = {0, 0, kExportW, kExportH};
    FillRect(memDc, &fr, bg);
    DeleteObject(bg);

    auto oldBrush = SelectObject(memDc, GetStockObject(DC_BRUSH));
    auto oldPen = SelectObject(memDc, GetStockObject(NULL_PEN));

    // Star chart look: halo/glow scale with the star's size, colour/brightness
    // with its magnitude. Only the bright core satisfies the scanner's
    // is_cyan() threshold (R<80, G>100, B>100, B>R+20, G>R+20); the halo and
    // mid-glow are deliberately non-cyan so they never register as particles.
    const int pr = 2; // bright cyan core (GDI fills ~4px, resolvable by scanner)

    auto mix = [](int bg, int target, double a) {
        return static_cast<int>(bg + (target - bg) * a);
    };

    // Two-pass drawing so no core is ever covered by a neighbour's halo:
    // pass 1 draws every halo/glow, pass 2 draws every bright core on top.
    // NOTE: GDI Ellipse treats the right/bottom edge as exclusive, so the
    // drawn disc is centred 0.5px up-left of the given centre; +1 on x2/y2
    // compensates so the detected centroid lands exactly on (sx, sy).
    for (const auto &p : particles) {
        int sx = static_cast<int>(p.sx) + kMargin;
        int sy = static_cast<int>(p.sy) + kMargin;
        int grOuter = static_cast<int>(5.5 * p.size) + 2; // 5..9
        int grMid = static_cast<int>(2.8 * p.size) + 1;   // 2..4
        double b = p.alpha * p.bright;

        // Outer halo: faint blue-violet, NOT cyan (G stays < 100).
        SetDCBrushColor(memDc, RGB(mix(10, 26, b),
                                   mix(10, 40, b),
                                   mix(10, 86, b)));
        Ellipse(memDc, sx - grOuter, sy - grOuter,
                sx + grOuter + 1, sy + grOuter + 1);

        // Mid glow: blue, still NOT cyan (G <= 100 at full brightness).
        SetDCBrushColor(memDc, RGB(mix(10, 0, b),
                                   mix(10, 90, b),
                                   mix(10, 128, b)));
        Ellipse(memDc, sx - grMid, sy - grMid,
                sx + grMid + 1, sy + grMid + 1);
    }
    for (const auto &p : particles) {
        int sx = static_cast<int>(p.sx) + kMargin;
        int sy = static_cast<int>(p.sy) + kMargin;
        // Bright cyan core (what the scanner detects), fixed full brightness.
        SetDCBrushColor(memDc, RGB(0, 190, 215));
        Ellipse(memDc, sx - pr, sy - pr, sx + pr + 1, sy + pr + 1);
    }

    // Corner markers: bright purple squares inside the black margin, so they
    // never overlap particles. Marked frames are scannable.
    if (markerCount > 0) {
        HBRUSH markerBrush = CreateSolidBrush(RGB(kMarkerR, kMarkerG, kMarkerB));
        auto oldM = SelectObject(memDc, markerBrush);
        const int corners[4][2] = {
            {kMarkerInset, kMarkerInset},                       // TL
            {kExportW - kMarkerInset - kMarkerSize, kMarkerInset}, // TR
            {kMarkerInset, kExportH - kMarkerInset - kMarkerSize}, // BL
            {kExportW - kMarkerInset - kMarkerSize, kExportH - kMarkerInset - kMarkerSize}, // BR
        };
        for (int c = 0; c < markerCount && c < 4; c++)
            Rectangle(memDc, corners[c][0], corners[c][1],
                      corners[c][0] + kMarkerSize, corners[c][1] + kMarkerSize);
        SelectObject(memDc, oldM);
        DeleteObject(markerBrush);
    }

    SelectObject(memDc, oldBrush);
    SelectObject(memDc, oldPen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kExportW;
    bi.bmiHeader.biHeight = -kExportH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD size = ((kExportW * 3 + 3) / 4) * 4 * kExportH;
    std::vector<uint8_t> pixels(size);
    GetDIBits(memDc, bmp, 0, kExportH, pixels.data(), &bi, DIB_RGB_COLORS);

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42;
    bfh.bfSize = 54 + size;
    bfh.bfOffBits = 54;

    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(&bfh, 1, sizeof(bfh), f);
        fwrite(&bi.bmiHeader, 1, sizeof(bi.bmiHeader), f);
        fwrite(pixels.data(), 1, size, f);
        fclose(f);
    }

    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, hdc);
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
    FILE *meta = fopen((outDir + "/anim_meta.txt").c_str(), "w");
    for (size_t d = 0; d < frameBytesList.size(); d++) {
        int pureFrames = kAnimFramesPerDataFrame - transition;
        for (int a = 0; a < pureFrames; a++) {
            double t = totalAnim / kFps; // continuous timeline across all frames
            auto frame = encoder.encodeSingleFrame(frameBytesList[d], t);

            char name[64];
            std::snprintf(name, sizeof(name), "anim_%03d.bmp", totalAnim);
            render_to_bmp(build_frame(frame, t, driftNoise), kMarkerCountFull,
                          outDir + "/" + name);

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
                render_to_bmp(build_morph(frameA, frameB, t, morphT, driftNoise), 0,
                              outDir + "/" + name);

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

