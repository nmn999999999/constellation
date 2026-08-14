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
static const int kExportW = kGridCols * 8; // 480
static const int kExportH = kGridRows * 8; // 480
static const double kScale = 8.0;
static const double kFps = 30.0;
static const int kAnimFramesPerDataFrame = 30;

// Corner marker geometry: a 4x4 bright-purple square near each corner of the
// export image. Purple is deliberately NOT cyan so it never registers as a
// particle; the scanner uses it to identify scannable frames.
static const int kMarkerSize = 4;
static const int kMarkerInset = 1;
static const int kMarkerCountFull = 4; // all four corners on scannable frames
static const int kMarkerR = 210, kMarkerG = 50, kMarkerB = 230;

struct DrawParticle {
    double sx, sy; // screen coordinates (pixels)
    double alpha;  // 0..1 brightness
};

// Deterministic pseudo-random angle for a grid index (used for the drift
// direction of particles entering/leaving during a morph).
static double hash_angle(int idx) {
    unsigned int h = static_cast<unsigned int>(idx) * 2654435761u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return static_cast<double>(h % 3600) / 10.0 * 3.14159265358979323846 / 180.0;
}

// Perlin drift: every particle floats continuously inside its own grid cell
// with an independent phase, so the whole field looks like drifting star dust
// even on pure (scannable) frames. Amplitude 0.35 grid units (~2.8 px) is
// clearly visible yet stays safely inside the cell (±0.5) for the decoder.
static const double kDriftAmp = 0.35;

static std::pair<double, double> drift_offset(const PerlinNoise &noise,
                                              int col, int row,
                                              double t, int phase) {
    double nx = noise.fbm(col * 0.05 + t * 0.15, row * 0.05 + phase * 0.13, 3);
    double ny = noise.fbm(col * 0.05 + phase * 0.13 + 7.7,
                          row * 0.05 + t * 0.15, 3);
    return {nx * kDriftAmp, ny * kDriftAmp};
}

// Build the draw list of a pure frame: every particle sits at its grid
// position plus the continuous Perlin drift (full brightness).
static std::vector<DrawParticle> build_frame(const EncodedFrame &frame, double t,
                                             const PerlinNoise &noise) {
    std::vector<DrawParticle> out;
    out.reserve(static_cast<size_t>(frame.particleCount));
    for (int i = 0; i < frame.particleCount; i++) {
        double x = frame.particles[i * 2], y = frame.particles[i * 2 + 1];
        int col = static_cast<int>(std::floor(x));
        int row = static_cast<int>(std::floor(y));
        auto [dx, dy] = drift_offset(noise, col, row, t, i);
        out.push_back({(x + dx) * kScale, (y + dy) * kScale, 1.0});
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
        auto [ox, oy] = drift_offset(noise, cr.first, cr.second, t, idx);
        auto it = posB.find(idx);
        if (it != posB.end()) {
            double bx = pa.first + (it->second.first - pa.first) * morphT;
            double by = pa.second + (it->second.second - pa.second) * morphT;
            out.push_back({(bx + ox) * kScale, (by + oy) * kScale, 1.0});
        } else {
            double ang = hash_angle(idx);
            double d = driftPx * morphT;
            out.push_back({(pa.first + std::cos(ang) * d + ox) * kScale,
                           (pa.second + std::sin(ang) * d + oy) * kScale,
                           1.0 - morphT});
        }
    }
    // Particles only in B: fade in while drifting in from the drift distance.
    for (const auto &kv : posB) {
        const int idx = kv.first;
        if (posA.count(idx)) continue;
        const std::pair<double, double> &pb = kv.second;
        auto cr = gridColRow(pb.first, pb.second);
        auto [ox, oy] = drift_offset(noise, cr.first, cr.second, t, idx);
        double ang = hash_angle(idx + 7);
        double d = driftPx * (1.0 - morphT);
        out.push_back({(pb.first - std::cos(ang) * d + ox) * kScale,
                       (pb.second - std::sin(ang) * d + oy) * kScale,
                       morphT});
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

    const int pr = 2; // max(int(0.35 * scale), 2)
    const int gr = 4; // pr * 2

    auto mix = [](int bg, int target, double a) {
        return static_cast<int>(bg + (target - bg) * a);
    };

    for (const auto &p : particles) {
        int glowR = mix(10, 0, p.alpha), glowG = mix(10, 100, p.alpha),
            glowB = mix(10, 130, p.alpha);
        int coreR = mix(10, 0, p.alpha), coreG = mix(10, 188, p.alpha),
            coreB = mix(10, 212, p.alpha);
        int sx = static_cast<int>(p.sx), sy = static_cast<int>(p.sy);

        SetDCBrushColor(memDc, RGB(glowR, glowG, glowB));
        Ellipse(memDc, sx - gr, sy - gr, sx + gr, sy + gr);
        SetDCBrushColor(memDc, RGB(coreR, coreG, coreB));
        Ellipse(memDc, sx - pr, sy - pr, sx + pr, sy + pr);
    }

    // Corner markers: bright purple squares. Marked frames are scannable.
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
