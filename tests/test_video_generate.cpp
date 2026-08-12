// Multi-frame animation generator: encodes a large payload into multiple data
// frames, renders each one at several animation times (Perlin drift), and
// writes an export-style BMP sequence suitable for ffmpeg video composition.
//
// Usage: test_video_generate.exe <output_dir> [payload_bytes] [transition_frames] [ecc]
//   transition_frames > 0 crossfades between consecutive data frames so the
//   video flows without hard cuts (transition frames are not decodable by
//   design; the per-data-frame redundancy still guarantees recovery).
//   "ecc" enables Hamming (7,4) error correction on the payload.
#include "particle_codec/codec.h"
#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#define NOMINMAX
#include <windows.h>

using namespace particle_codec;

static const int kGridCols = 60, kGridRows = 60;
static const int kExportW = kGridCols * 8; // 480
static const int kExportH = kGridRows * 8; // 480
static const double kScale = 8.0;
static const double kFps = 30.0;
static const int kAnimFramesPerDataFrame = 30;

// Viewer export style: dark navy background, cyan glow + brighter core.
static void render_frame_to_bmp(const EncodedFrame &frame, const std::string &path,
                                double alpha = 1.0) {
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
    int glowR = mix(10, 0, alpha), glowG = mix(10, 100, alpha), glowB = mix(10, 130, alpha);
    int coreR = mix(10, 0, alpha), coreG = mix(10, 188, alpha), coreB = mix(10, 212, alpha);

    for (int i = 0; i < frame.particleCount; i++) {
        double sx = frame.particles[i * 2] * kScale;
        double sy = frame.particles[i * 2 + 1] * kScale;

        SetDCBrushColor(memDc, RGB(glowR, glowG, glowB));
        Ellipse(memDc, (int)(sx - gr), (int)(sy - gr), (int)(sx + gr), (int)(sy + gr));

        SetDCBrushColor(memDc, RGB(coreR, coreG, coreB));
        Ellipse(memDc, (int)(sx - pr), (int)(sy - pr), (int)(sx + pr), (int)(sy + pr));
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

// Crossfade: draw the outgoing frame dimmed and the incoming frame brightened
// on the same canvas, so data frames blend instead of hard-switching.
static void render_crossfade_to_bmp(const EncodedFrame &outFrame,
                                    const EncodedFrame &inFrame,
                                    double inAlpha,
                                    const std::string &path) {
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

    auto draw = [&](const EncodedFrame &frame, double alpha) {
        auto mix = [](int bg, int target, double a) {
            return static_cast<int>(bg + (target - bg) * a);
        };
        int glowR = mix(10, 0, alpha), glowG = mix(10, 100, alpha), glowB = mix(10, 130, alpha);
        int coreR = mix(10, 0, alpha), coreG = mix(10, 188, alpha), coreB = mix(10, 212, alpha);
        const int pr = 2, gr = 4;
        for (int i = 0; i < frame.particleCount; i++) {
            double sx = frame.particles[i * 2] * kScale;
            double sy = frame.particles[i * 2 + 1] * kScale;
            SetDCBrushColor(memDc, RGB(glowR, glowG, glowB));
            Ellipse(memDc, (int)(sx - gr), (int)(sy - gr), (int)(sx + gr), (int)(sy + gr));
            SetDCBrushColor(memDc, RGB(coreR, coreG, coreB));
            Ellipse(memDc, (int)(sx - pr), (int)(sy - pr), (int)(sx + pr), (int)(sy + pr));
        }
    };

    draw(outFrame, 1.0 - inAlpha);
    draw(inFrame, inAlpha);

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
        std::cout << "Crossfade: " << transition << " transition frame(s) between data frames"
                  << std::endl;
    }

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
    FILE *meta = fopen((outDir + "/anim_meta.txt").c_str(), "w");
    for (size_t d = 0; d < frameBytesList.size(); d++) {
        int pureFrames = kAnimFramesPerDataFrame - transition;
        for (int a = 0; a < pureFrames; a++) {
            double t = totalAnim / kFps; // continuous timeline across all frames
            auto frame = encoder.encodeSingleFrame(frameBytesList[d], t);

            char name[64];
            std::snprintf(name, sizeof(name), "anim_%03d.bmp", totalAnim);
            render_frame_to_bmp(frame, outDir + "/" + name);

            if (meta) {
                std::fprintf(meta, "%03d seq=%zu particles=%d t=%.2f\n",
                             totalAnim, d, frame.particleCount, t);
            }
            totalAnim++;
        }

        // Crossfade into the next data frame (skip after the last one).
        if (d + 1 < frameBytesList.size() && transition > 0) {
            for (int tr = 0; tr < transition; tr++) {
                double t = totalAnim / kFps;
                auto outFrame = encoder.encodeSingleFrame(frameBytesList[d], t);
                double inAlpha = static_cast<double>(tr + 1) / (transition + 1);
                auto inFrame = encoder.encodeSingleFrame(frameBytesList[d + 1], t);

                char name[64];
                std::snprintf(name, sizeof(name), "anim_%03d.bmp", totalAnim);
                render_crossfade_to_bmp(outFrame, inFrame, inAlpha, outDir + "/" + name);

                if (meta) {
                    std::fprintf(meta, "%03d transition %zu->%zu alpha=%.2f t=%.2f\n",
                                 totalAnim, d, d + 1, inAlpha, t);
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
