// Video-frame decoder: loads a directory of PNG/BMP frames extracted from a
// video, detects particles, decodes each frame, assembles the multi-frame
// payload and (optionally) verifies it against the original payload.bin.
//
// Usage: test_video_decode.exe <frame_dir> [payload.bin]
#include "particle_codec/codec.h"
#include "particle_codec/error.h"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#include "../demo/stb_image.h"

using namespace particle_codec;

struct Detection {
    double cx, cy;
    int area;
};

static inline bool is_cyan(unsigned char r, unsigned char g, unsigned char b) {
    return r < 80 && g > 100 && b > 100 && b > r + 20 && g > r + 20;
}

// Color flood-fill 8-neighbour clustering (same approach as decode_image.cpp).
static std::vector<Detection> detect_by_color(std::vector<uint8_t> &mask, int w, int h) {
    std::vector<Detection> detections;
    std::vector<int> stack;
    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (mask[idx] == 0) continue;

            double sumX = 0, sumY = 0;
            int count = 0;
            stack.clear();
            stack.push_back(idx);
            mask[idx] = 0;

            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % w;
                int cy = cur / w;
                sumX += cx;
                sumY += cy;
                count++;
                for (int d = 0; d < 8; d++) {
                    int nx = cx + dx8[d], ny = cy + dy8[d];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    int nidx = ny * w + nx;
                    if (mask[nidx] == 0) continue;
                    mask[nidx] = 0;
                    stack.push_back(nidx);
                }
            }
            if (count >= 3) detections.push_back({sumX / count, sumY / count, count});
        }
    }
    return detections;
}

// Export-style direct scaling (no centering): gx = pixelX * cols / width.
static std::vector<std::pair<double, double> > direct_grid(
    const std::vector<Detection> &detections, int w, int h, int cols, int rows) {
    double scaleX = static_cast<double>(w) / cols;
    double scaleY = static_cast<double>(h) / rows;
    std::vector<std::pair<double, double> > gp;
    std::vector<bool> cellUsed(cols * rows, false);
    for (auto &c: detections) {
        double gx = c.cx / scaleX;
        double gy = c.cy / scaleY;
        int col = std::clamp(static_cast<int>(std::floor(gx)), 0, cols - 1);
        int row = std::clamp(static_cast<int>(std::floor(gy)), 0, rows - 1);
        int idx = row * cols + col;
        if (!cellUsed[idx]) {
            cellUsed[idx] = true;
            gp.emplace_back(col + 0.5, row + 0.5);
        }
    }
    return gp;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test_video_decode.exe <frame_dir> [payload.bin]" << std::endl;
        return 1;
    }
    std::string dir = argv[1];
    std::string payloadPath = (argc > 2) ? argv[2] : "";

    const int gridCols = 60, gridRows = 60;
    ParticleCodec codec("particle_codec", gridCols, gridRows);
    FrameAssembler assembler;

    std::vector<std::string> files;
    for (const auto &entry: std::filesystem::directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());

    std::cout << "=== Video Frame Decode ===" << std::endl;
    std::cout << "Frames found: " << files.size() << " in " << dir << std::endl;

    int decodedFrames = 0;     // frames whose particles yielded a valid frame
    int freshAdds = 0;         // first successful add for a data-frame seq
    int failFrames = 0;        // frames that could not be decoded at all
    std::vector<int> seqSuccess(64, 0); // data-frame success counts
    std::vector<std::string> failByStage;

    for (size_t fi = 0; fi < files.size(); fi++) {
        int w = 0, h = 0, channels = 0;
        unsigned char *img = stbi_load(files[fi].c_str(), &w, &h, &channels, 3);
        if (!img) {
            failFrames++;
            failByStage.push_back("load-failed");
            continue;
        }

        int total = w * h;
        std::vector<uint8_t> mask(total, 0);
        for (int i = 0; i < total; i++)
            mask[i] = is_cyan(img[i * 3], img[i * 3 + 1], img[i * 3 + 2]) ? 1 : 0;
        stbi_image_free(img);

        auto detections = detect_by_color(mask, w, h);
        auto gp = direct_grid(detections, w, h, gridCols, gridRows);

        auto result = codec.decodeCentroidsDetailed(gp);
        if (!result.ok()) {
            failFrames++;
            failByStage.push_back(errorName(result.error().code));
            continue;
        }

        auto header = FrameHeader::tryParse(result.value());
        if (!header) {
            failFrames++;
            failByStage.push_back("no-header");
            continue;
        }

        std::vector<uint8_t> payload(
            result.value().begin() + FrameHeader::totalSize,
            result.value().begin() + FrameHeader::totalSize + header->payloadLength);
        decodedFrames++;
        if (header->seq >= 0 && header->seq < static_cast<int>(seqSuccess.size()))
            seqSuccess[header->seq]++;
        auto added = assembler.addFrameEx(header->seq, payload);
        if (added.ok()) {
            freshAdds++;
        }
    }

    std::cout << "Frames decoded successfully: " << decodedFrames
              << " / " << files.size()
              << " (fresh data-frame adds: " << freshAdds << ")" << std::endl;

    std::cout << "Per data-frame coverage (animation frames decoded for each seq):" << std::endl;
    int distinct = 0;
    for (size_t s = 0; s < seqSuccess.size(); s++) {
        if (seqSuccess[s] <= 0) continue;
        std::cout << "  seq " << s << ": " << seqSuccess[s] << " frame(s)" << std::endl;
        distinct++;
    }
    if (failFrames > 0) {
        std::cout << "Failed frames: " << failFrames << std::endl;
        std::cout << "  (first failures: ";
        for (size_t i = 0; i < 6 && i < failByStage.size(); i++)
            std::cout << failByStage[i] << " ";
        std::cout << "...)" << std::endl;
    }

    auto assembled = assembler.extractAll();
    if (!assembled) {
        std::cerr << "RESULT: FAIL - no data assembled" << std::endl;
        return 1;
    }

    std::cout << "Assembled " << assembled->size() << " bytes from "
              << distinct << " distinct data frame(s)" << std::endl;

    if (!payloadPath.empty()) {
        FILE *f = fopen(payloadPath.c_str(), "rb");
        if (!f) {
            std::cerr << "RESULT: FAIL - cannot open payload " << payloadPath << std::endl;
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> original(static_cast<size_t>(len));
        if (len > 0) fread(original.data(), 1, static_cast<size_t>(len), f);
        fclose(f);

        bool match = assembled->size() == original.size() &&
                     std::equal(assembled->begin(), assembled->end(), original.begin());
        std::cout << "Payload verification: " << (match ? "PASS" : "FAIL")
                  << " (" << assembled->size() << " vs " << original.size() << " bytes)" << std::endl;
        return match ? 0 : 2;
    }

    std::cout << "RESULT: OK - " << assembled->size() << " bytes assembled" << std::endl;
    return 0;
}
