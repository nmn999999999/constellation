// Video-frame decoder: loads a directory of PNG/BMP frames extracted from a
// video, detects particles, decodes each frame, assembles the multi-frame
// payload and (optionally) verifies it against the original payload.bin.
//
// Usage: test_video_decode.exe <frame_dir> [payload.bin]
#include "particle_codec/codec.h"
#include "particle_codec/error.h"
#include "particle_codec/grid_calibrator.h"
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

    // Multi-frame fusion: for every data-frame sequence number, collect the
    // mapped centroids of all animation frames (even ones whose CRC failed) so
    // missing particles on individual frames are filled by the others.
    std::vector<std::vector<std::pair<double, double> > > fusion(64);
    std::vector<int> fusionFrames(64, 0);
    // Video geometry is static across frames: once the first frame is
    // calibrated, reuse that exact transform (including orientation and
    // translation) so fused cells stay consistent.
    GridCalibrator::Affine globalMap;
    bool haveGlobalMap = false;

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
    int calibratedFrames = 0;  // decoded only after geometry calibration
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

        std::vector<std::pair<double, double> > centroids;
        for (const auto &d: detections)
            centroids.emplace_back(d.cx, d.cy);

        std::vector<std::pair<double, double> > finalPoints;
        auto result = Result<std::vector<uint8_t> >::failure(
            makeError(ErrorCode::NoParticles, ""));
        bool usedCalibrated = false;

        if (haveGlobalMap) {
            // Video geometry is static: reuse the transform locked in by the
            // first frame so fused cells stay consistent across frames.
            finalPoints.reserve(centroids.size());
            for (const auto &c: centroids)
                finalPoints.push_back(globalMap.map(c.first, c.second));
            result = codec.decodeCentroidsDetailed(finalPoints);
            usedCalibrated = true;
        } else {
            finalPoints = gp;
            result = codec.decodeCentroidsDetailed(gp);
            if (!result.ok() && detections.size() >= 16) {
                // First frame: calibrate and lock in the transform.
                auto cal = GridCalibrator::calibrate(centroids, gridCols, gridRows);
                if (cal.valid) {
                    GridCalibrator::Affine variants[4] = {
                        cal, cal.rotated(), cal.rotated().rotated(),
                        cal.rotated().rotated().rotated()};
                    std::vector<std::pair<double, double> > bestMapped;
                    int bestVariant = -1;
                    ErrorCode bestErr = ErrorCode::None;
                    for (int v = 0; v < 4; v++) {
                        std::vector<std::pair<double, double> > mapped;
                        for (const auto &c: centroids)
                            mapped.push_back(variants[v].map(c.first, c.second));
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
                        result = codec.decodeCentroidsDetailed(mapped);
                        if (result.ok()) {
                            finalPoints = mapped;
                            globalMap = variants[v];
                            globalMap.c -= sx;
                            globalMap.f -= sy;
                            haveGlobalMap = true;
                            break;
                        }
                        if (result.error().code == ErrorCode::CrcMismatch &&
                            bestErr != ErrorCode::CrcMismatch) {
                            bestErr = ErrorCode::CrcMismatch;
                            bestMapped = mapped;
                            bestVariant = v;
                        } else if (bestErr == ErrorCode::None && bestMapped.empty()) {
                            bestErr = result.error().code;
                            bestMapped = mapped;
                            bestVariant = v;
                        }
                    }
                    if (!result.ok() && !bestMapped.empty() &&
                        bestErr == ErrorCode::CrcMismatch) {
                        // Lock in the geometry even though this frame's CRC
                        // failed: the sync word matched, so orientation and
                        // translation are trustworthy for the whole video.
                        finalPoints = bestMapped;
                        globalMap = variants[bestVariant];
                        double minX = 1e18, minY = 1e18;
                        for (const auto &c: centroids) {
                            auto m = globalMap.map(c.first, c.second);
                            minX = std::min(minX, m.first);
                            minY = std::min(minY, m.second);
                        }
                        globalMap.c -= std::round(minX - 0.5);
                        globalMap.f -= std::round(minY - 0.5);
                        haveGlobalMap = true;
                        usedCalibrated = true;
                    }
                }
            }
        }

        // Record the frame's mapped centroids under its sequence number even
        // when the CRC failed (sequence comes from sync+length only).
        auto raw = codec.decodeCentroidsRaw(finalPoints);
        if (raw) {
            auto rh = FrameHeader::tryParse(raw.value());
            if (rh && rh->seq >= 0 && rh->seq < static_cast<int>(fusion.size())) {
                for (const auto &p: finalPoints)
                    fusion[rh->seq].push_back(p);
                fusionFrames[rh->seq]++;
            }
        }

        if (!result.ok()) {
            failFrames++;
            failByStage.push_back("f" + std::to_string(fi) + ":" +
                                  errorName(result.error().code));
            continue;
        }
        if (usedCalibrated) calibratedFrames++;

        auto header = FrameHeader::tryParse(result.value());
        if (!header) {
            failFrames++;
            failByStage.push_back("f" + std::to_string(fi) + ":no-header");
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
    if (calibratedFrames > 0)
        std::cout << "Decoded via geometry calibration: " << calibratedFrames
                  << " frame(s)" << std::endl;

    std::cout << "Per data-frame coverage (animation frames decoded for each seq):" << std::endl;
    int distinct = 0;
    for (size_t s = 0; s < seqSuccess.size(); s++) {
        if (seqSuccess[s] <= 0) continue;
        std::cout << "  seq " << s << ": " << seqSuccess[s] << " frame(s)" << std::endl;
        distinct++;
    }
    if (failFrames > 0) {
        std::cout << "Failed frames: " << failFrames << std::endl;
        std::cout << "  failures: ";
        for (size_t i = 0; i < failByStage.size() && i < 40; i++)
            std::cout << failByStage[i] << " ";
        if (failByStage.size() > 40) std::cout << "...";
        std::cout << std::endl;
    }

    // Fuse collected frames per sequence: average the mapped centroids of each
    // grid cell across all animation frames, then decode the fused set.
    int fusedSeqs = 0, fusedAdds = 0;
    for (int seq = 0; seq < static_cast<int>(fusion.size()); seq++) {
        if (fusion[seq].empty()) continue;

        std::vector<double> sumX(3600, 0), sumY(3600, 0);
        std::vector<int> cellCount(3600, 0);
        for (const auto &p: fusion[seq]) {
            int col = std::clamp(static_cast<int>(std::floor(p.first)), 0, gridCols - 1);
            int row = std::clamp(static_cast<int>(std::floor(p.second)), 0, gridRows - 1);
            int idx = row * gridCols + col;
            sumX[idx] += p.first;
            sumY[idx] += p.second;
            cellCount[idx]++;
        }

        std::vector<std::pair<double, double> > fused;
        for (int idx = 0; idx < 3600; idx++) {
            if (cellCount[idx] == 0) continue;
            // Require at least 2 votes: single-frame spurious cells (noise or
            // a particle snapped into a neighbouring cell) are filtered out,
            // while real particles appear in ~23 of 24 frames.
            if (cellCount[idx] < 2) continue;
            fused.emplace_back(sumX[idx] / cellCount[idx],
                               sumY[idx] / cellCount[idx]);
        }

        // The averaged centroids have much lower noise than any single frame,
        // so refine the geometry once more on the fused set to eliminate the
        // systematic cell offsets that made individual frames fail.
        auto fres = codec.decodeCentroidsDetailed(fused);
        if (!fres.ok()) {
            auto refined = GridCalibrator::calibrate(fused, gridCols, gridRows);
            if (refined.valid) {
                GridCalibrator::Affine variants[4] = {
                    refined, refined.rotated(), refined.rotated().rotated(),
                    refined.rotated().rotated().rotated()};
                for (int v = 0; v < 4 && !fres.ok(); v++) {
                    std::vector<std::pair<double, double> > remapped;
                    for (const auto &p: fused)
                        remapped.push_back(variants[v].map(p.first, p.second));
                    double minX = 1e18, minY = 1e18;
                    for (const auto &p: remapped) {
                        minX = std::min(minX, p.first);
                        minY = std::min(minY, p.second);
                    }
                    double sx = std::round(minX - 0.5), sy = std::round(minY - 0.5);
                    for (auto &p: remapped) {
                        p.first -= sx;
                        p.second -= sy;
                    }
                    fres = codec.decodeCentroidsDetailed(remapped);
                    if (fres.ok()) {
                        std::cout << "  fuse seq " << seq
                                  << ": recovered via refined geometry (variant "
                                  << v << ")\n";
                    }
                }
            }
        }
        if (!fres.ok()) {
            // ECC fallback: rebuild the raw frame (sync/length checked, CRC
            // skipped), Hamming-decode the payload, then verify the result
            // against the frame's original CRC field.
            auto raw = codec.decodeCentroidsRaw(fused);
            if (raw) {
                auto rh = FrameHeader::tryParse(raw.value());
                if (rh && rh->payloadLength > 0) {
                    std::vector<uint8_t> payload(
                        raw->begin() + FrameHeader::totalSize,
                        raw->begin() + FrameHeader::totalSize + rh->payloadLength);
                    auto corrected = HammingEncoder::decode(payload);
                    auto rebuilt = FrameBuilder::build(rh->seq, corrected);
                    bool crcOk = rebuilt.size() == raw->size();
                    for (int i = 0; crcOk && i < FrameHeader::crcSize; i++)
                        crcOk = rebuilt[FrameHeader::totalSize -
                                        FrameHeader::crcSize + i] ==
                                (*raw)[FrameHeader::totalSize -
                                       FrameHeader::crcSize + i];
                    if (crcOk) {
                        auto added = assembler.addFrameEx(rh->seq, corrected);
                        if (added.ok()) {
                            fusedSeqs++;
                            fusedAdds++;
                            seqSuccess[seq] = fusionFrames[seq];
                            std::cout << "  fuse seq " << seq
                                      << ": recovered via ECC\n";
                            continue;
                        }
                    }
                }
            }
            std::cout << "  fuse seq " << seq << ": FAILED ("
                      << errorName(fres.error().code) << ")\n";
            continue;
        }
        auto fh = FrameHeader::tryParse(fres.value());
        if (!fh) continue;
        std::vector<uint8_t> payload(
            fres.value().begin() + FrameHeader::totalSize,
            fres.value().begin() + FrameHeader::totalSize + fh->payloadLength);
        auto added = assembler.addFrameEx(seq, payload);
        if (added.ok()) {
            fusedSeqs++;
            fusedAdds++;
            seqSuccess[seq] = fusionFrames[seq];
        }
    }
    std::cout << "Fusion: " << fusedSeqs << " seq(s) recovered from "
              << fusedAdds << " fused frame set(s)" << std::endl;

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
