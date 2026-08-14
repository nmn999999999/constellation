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
#include <map>
#include <unordered_map>
#include <cstdio>

#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#endif

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

// Color flood-fill 8-neighbour clustering with intensity-weighted centroids:
// bright core pixels weigh more than the dim glow, so the centroid lands on
// the particle core even under compression blur.
static std::vector<Detection> detect_by_color(std::vector<uint8_t> &mask, int w, int h,
                                              const std::vector<float> *weights = nullptr) {
    std::vector<Detection> detections;
    std::vector<int> stack;
    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (mask[idx] == 0) continue;

            double sumX = 0, sumY = 0, sumW = 0;
            int count = 0;
            stack.clear();
            stack.push_back(idx);
            mask[idx] = 0;

            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % w;
                int cy = cur / w;
                double wt = weights ? (*weights)[cur] : 1.0;
                sumX += cx * wt;
                sumY += cy * wt;
                sumW += wt;
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
            if (count >= 3 && sumW > 0)
                detections.push_back({sumX / sumW, sumY / sumW, count});
        }
    }
    return detections;
}

// Export-style direct scaling (no centering): gx = pixelX * cols / width.
// `margin` is the black border (8px) that carries the corner markers; it is
// subtracted so grid scaling stays exact even on marked (v2) frames.
static std::vector<std::pair<double, double> > direct_grid(
    const std::vector<Detection> &detections, int w, int h, int cols, int rows,
    int margin = 0) {
    double scaleX = static_cast<double>(w - 2 * margin) / cols;
    double scaleY = static_cast<double>(h - 2 * margin) / rows;
    std::vector<std::pair<double, double> > gp;
    std::vector<bool> cellUsed(cols * rows, false);
    for (auto &c: detections) {
        double gx = (c.cx - margin) / scaleX;
        double gy = (c.cy - margin) / scaleY;
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

// Corner-marker detection: the generator draws a 4x4 bright-purple square in
// the black margin of each corner of scannable frames (transition/morph frames
// have none). Purple is intentionally not cyan, so it never registers as a
// particle. Returns the number of corners carrying a marker (0..4).
static int detect_corner_markers(const unsigned char *img, int w, int h) {
    auto cornerHas = [&](int x0, int y0) {
        int purple = 0;
        for (int dy = 0; dy < 6 && y0 + dy < h; dy++)
            for (int dx = 0; dx < 6 && x0 + dx < w; dx++) {
                const unsigned char *p = img + ((y0 + dy) * w + (x0 + dx)) * 3;
                // bright purple: high R, high B, low G, clearly red-shifted
                if (p[0] > 140 && p[2] > 140 && p[1] < 110 && p[0] > p[1] + 60)
                    purple++;
            }
        return purple >= 4; // at least 4 purple pixels inside the 6x6 window
    };
    // Marker spans (inset, inset)..(inset+4) with inset=2 inside the 8px margin.
    int n = 0;
    if (cornerHas(2, 2)) n++;
    if (cornerHas(w - 6, 2)) n++;
    if (cornerHas(2, h - 6)) n++;
    if (cornerHas(w - 6, h - 6)) n++;
    return n;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test_video_decode.exe <frame_dir> [payload.bin]" << std::endl;
        return 1;
    }
    std::string dir = argv[1];
    std::string payloadPath = (argc > 2) ? argv[2] : "";

    const int gridCols = 60, gridRows = 60;
    const double kScalePx = 8.0; // px per grid cell in the export image
    ParticleCodec codec("particle_codec", gridCols, gridRows);
    FrameAssembler assembler;

    // Multi-frame fusion: for every data-frame sequence number, collect the
    // mapped centroids of all animation frames (even ones whose CRC failed) so
    // missing particles on individual frames are filled by the others.
    std::vector<std::vector<std::pair<double, double> > > fusion(64);
    std::vector<int> fusionFrames(64, 0);
    std::vector<std::pair<double, double> > allRaw; // raw image centroids of every frame
    // Video geometry is static across frames: once the first frame is
    // calibrated, reuse that exact transform (including orientation and
    // translation) so fused cells stay consistent.
    GridCalibrator::Affine globalMap;
    bool haveGlobalMap = false;

    // List image files. On Windows use FindFirstFileA (ANSI) so Chinese paths
    // work; elsewhere use std::filesystem (no narrow->wide conversion issue).
    std::vector<std::string> files;
#ifdef _WIN32
    {
        std::string pattern = dir;
        if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
            pattern += '\\';
        pattern += '*';
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::string name(fd.cFileName);
                std::string ext = name.size() >= 4 ? name.substr(name.size() - 4) : "";
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" ||
                    (name.size() >= 5 && name.substr(name.size() - 5) == ".jpeg")) {
                    files.push_back(dir + "\\" + name);
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
#else
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg") {
            files.push_back(entry.path().string());
        }
    }
#endif
    std::sort(files.begin(), files.end());

    std::cout << "=== Video Frame Decode ===" << std::endl;
    std::cout << "Frames found: " << files.size() << " in " << dir << std::endl;

    int decodedFrames = 0;     // frames whose particles yielded a valid frame
    int calibratedFrames = 0;  // decoded only after geometry calibration
    int freshAdds = 0;         // first successful add for a data-frame seq
    int failFrames = 0;        // frames that could not be decoded at all
    int scannableFrames = 0;   // frames carrying full corner markers
    int skippedMorphFrames = 0;// transition/morph frames skipped by markers
    std::vector<int> seqSuccess(64, 0); // data-frame success counts
    std::vector<std::string> failByStage;
    int globalW = 0, globalH = 0; // image size of the first loaded frame
    int gMargin = 0;              // black margin detected on scannable frames

    for (size_t fi = 0; fi < files.size(); fi++) {
        int w = 0, h = 0, channels = 0;
        unsigned char *img = stbi_load(files[fi].c_str(), &w, &h, &channels, 3);
        if (!img) {
            failFrames++;
            failByStage.push_back("load-failed");
            continue;
        }
        if (globalW == 0) {
            globalW = w;
            globalH = h;
        }

        // Scannable-frame gating: the generator marks pure data frames with 4
        // corner markers and leaves morph transitions unmarked. Only grab
        // frames with >= 3 markers (allowing one corner lost to compression);
        // everything else is a transition and is skipped automatically.
        int markers = detect_corner_markers(img, w, h);
        if (markers < 3) {
            stbi_image_free(img);
            skippedMorphFrames++;
            failByStage.push_back("morph-skipped");
            continue;
        }
        scannableFrames++;

        // Marked frames come from the v2 generator: a black margin of
        // kMarginPx pixels around the grid carries the markers. Skip it so the
        // margin (and any marker bleed after compression) never counts as a
        // particle and grid scaling stays correct.
        const int margin = 8;
        gMargin = margin;
        int total = w * h;
        std::vector<uint8_t> mask(total, 0);
        std::vector<float> weights(total, 0);
        for (int y = margin; y < h - margin; y++) {
            for (int x = margin; x < w - margin; x++) {
                int i = y * w + x;
                if (is_cyan(img[i * 3], img[i * 3 + 1], img[i * 3 + 2])) {
                    mask[i] = 1;
                    weights[i] = static_cast<float>(img[i * 3 + 1] + img[i * 3 + 2]);
                }
            }
        }
        stbi_image_free(img);

        auto detections = detect_by_color(mask, w, h, &weights);
        auto gp = direct_grid(detections, w, h, gridCols, gridRows, margin);

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
                // Store raw image centroids; mapping happens after the joint
                // geometry optimization so all frames share one transform.
                for (const auto &c: centroids)
                    fusion[rh->seq].push_back(c);
                fusionFrames[rh->seq]++;
            }
        }
        for (const auto &c: centroids)
            allRaw.push_back(c);

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

    std::cout << "Scannable frames (4 corner markers): " << scannableFrames
              << " / " << files.size() << std::endl;
    if (skippedMorphFrames > 0)
        std::cout << "Morph/transition frames skipped automatically: "
                  << skippedMorphFrames << std::endl;
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

    // Bundle adjustment: every frame shares one affine geometry. Optimize the
    // transform over the raw image centroids of all frames so detection noise
    // averages out and the systematic per-cell offsets that sink individual
    // frames disappear.
    GridCalibrator::Affine opt;
    opt.valid = true;
    if (haveGlobalMap) {
        opt = globalMap;
    } else if (globalW > 0) {
        // Axis-aligned fallback: pixel -> unit grid by image size, accounting
        // for the black margin (8px) that carries the corner markers.
        double effW = static_cast<double>(globalW) - 2 * gMargin;
        double effH = static_cast<double>(globalH) - 2 * gMargin;
        opt.a = static_cast<double>(gridCols) / effW;
        opt.e = static_cast<double>(gridRows) / effH;
        opt.c = -gMargin * opt.a;
        opt.f = -gMargin * opt.e;
        opt.b = opt.d = 0;
    }
    int baIter = 0;
    if (allRaw.size() >= 64) {
        for (int iter = 0; iter < 8; iter++) {
            std::map<int, std::pair<double, double> > cellSum;
            std::map<int, int> cellCount;
            for (const auto &p: allRaw) {
                auto m = opt.map(p.first, p.second);
                int col = std::clamp(static_cast<int>(std::floor(m.first)),
                                     0, gridCols - 1);
                int row = std::clamp(static_cast<int>(std::floor(m.second)),
                                     0, gridRows - 1);
                double ex = m.first - (col + 0.5), ey = m.second - (row + 0.5);
                // Drift budget is ~0.42 grid units; keep 0.45 so animated
                // particles (with margin-shifted coords) still vote.
                if (ex * ex + ey * ey > 0.45 * 0.45) continue;
                int idx = row * gridCols + col;
                cellSum[idx].first += p.first; // average in image space
                cellSum[idx].second += p.second;
                cellCount[idx]++;
            }
            std::vector<std::pair<double, double> > pts, grd;
            for (const auto &kv: cellSum) {
                int idx = kv.first;
                if (cellCount[idx] < 2) continue;
                pts.emplace_back(kv.second.first / cellCount[idx],
                                 kv.second.second / cellCount[idx]);
                grd.emplace_back((idx % gridCols) + 0.5, (idx / gridCols) + 0.5);
            }
            if (pts.size() < 9) break;
            GridCalibrator::Affine next = opt;
            if (!GridCalibrator::fitAffine(pts, grd, next)) break;
            double delta = std::abs(next.a - opt.a) + std::abs(next.b - opt.b) +
                           std::abs(next.c - opt.c) + std::abs(next.d - opt.d) +
                           std::abs(next.e - opt.e) + std::abs(next.f - opt.f);
            opt = next;
            baIter = iter + 1;
            if (delta < 1e-7) break;
        }
    }
    std::cout << "Bundle adjustment: " << baIter << " iteration(s)" << std::endl;

    // Fuse collected frames per sequence using the optimized transform, then
    // decode (with ECC fallback).
    int fusedSeqs = 0, fusedAdds = 0;
    // Fusion maps pixels to the grid the same way the single-frame path does
    // (margin-aware direct scaling). The bundle-adjusted `opt` can absorb the
    // global drift's systematic offset and push edge particles across cell
    // boundaries, so do not use it here.
    double fuseScale = (globalW - 2 * gMargin > 0)
                           ? static_cast<double>(globalW - 2 * gMargin) / gridCols
                           : kScalePx;
    for (int seq = 0; seq < static_cast<int>(fusion.size()); seq++) {
        if (fusion[seq].empty()) continue;

        std::map<int, std::pair<double, double> > cellSum;
        std::map<int, int> cellCount;
        for (const auto &p: fusion[seq]) {
            double gx = (p.first - gMargin) / fuseScale;
            double gy = (p.second - gMargin) / fuseScale;
            int col = std::clamp(static_cast<int>(std::floor(gx)), 0, gridCols - 1);
            int row = std::clamp(static_cast<int>(std::floor(gy)), 0, gridRows - 1);
            double ex = gx - (col + 0.5), ey = gy - (row + 0.5);
            // Drift budget is ~0.42 grid units; keep 0.45 so animated
            // particles (with margin-shifted coords) still vote.
            if (ex * ex + ey * ey > 0.45 * 0.45) continue;
            int idx = row * gridCols + col;
            cellSum[idx].first += p.first;
            cellSum[idx].second += p.second;
            cellCount[idx]++;
        }

        std::vector<std::pair<double, double> > fused;
        for (const auto &kv: cellSum) {
            int idx = kv.first;
            int cc = cellCount[idx];
            // >= 2 votes filters single-frame spurious cells (noise or a
            // particle snapped into a neighbour); real particles appear in
            // ~23 of 24 frames.
            if (cc < 2) continue;
            // Map the averaged pixel centroid to grid coords with the same
            // margin-aware formula as the single-frame path (NOT opt, whose
            // bundle adjustment absorbs the global drift's systematic offset).
            double cx = kv.second.first / cc;
            double cy = kv.second.second / cc;
            fused.emplace_back((cx - gMargin) / fuseScale,
                               (cy - gMargin) / fuseScale);
        }
        if (fused.empty()) continue;

        auto fres = codec.decodeCentroidsDetailed(fused);
        if (!fres.ok()) {
            // CRC-guided bit repair: the fused frame is usually off by only
            // 1-2 bits, so brute-force flip 1 (then 2) bits until the CRC
            // passes. A single pass is a few thousand cheap CRC checks.
            auto repair = [&](const std::vector<uint8_t> &frame)
                -> std::vector<uint8_t> {
                if (frame.size() < 5) return {};
                const size_t nbits = frame.size() * 8;
                auto flip1 = [&](size_t i) {
                    std::vector<uint8_t> f = frame;
                    f[i / 8] ^= static_cast<uint8_t>(1 << (7 - (i % 8)));
                    return f;
                };
                for (size_t i = 0; i < nbits; i++)
                    if (FrameBuilder::verifyCrc(flip1(i))) return flip1(i);
                for (size_t i = 0; i < nbits; i++) {
                    std::vector<uint8_t> f = flip1(i);
                    for (size_t j = i + 1; j < nbits; j++) {
                        f[j / 8] ^= static_cast<uint8_t>(1 << (7 - (j % 8)));
                        if (FrameBuilder::verifyCrc(f)) return f;
                        f[j / 8] ^= static_cast<uint8_t>(1 << (7 - (j % 8)));
                    }
                }
                return {};
            };

            auto repaired = repair(codec.decodeCentroidsRaw(fused).value_or(
                std::vector<uint8_t>()));
            if (!repaired.empty()) {
                auto rh = FrameHeader::tryParse(repaired);
                if (rh) {
                    std::vector<uint8_t> payload(
                        repaired.begin() + FrameHeader::totalSize,
                        repaired.begin() + FrameHeader::totalSize +
                            rh->payloadLength);
                    auto added = assembler.addFrameEx(rh->seq, payload);
                    if (added.ok()) {
                        fusedSeqs++;
                        fusedAdds++;
                        seqSuccess[seq] = fusionFrames[seq];
                        std::cout << "  fuse seq " << seq
                                  << ": recovered via CRC-guided bit repair\n";
                        continue;
                    }
                }
            }
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
                      << errorName(fres.error().code) << ", fused cells="
                      << fused.size() << ")\n";
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
        } else {
            // Single frames already covered this seq; fusion decoded it fine
            // but the duplicate add was rejected.
            std::cout << "  fuse seq " << seq
                      << ": decoded OK (dup add rejected)\n";
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
