// Precision benchmark: encode -> render (viewer export style) -> detect -> decode.
// Measures particle recall, grid-cell accuracy, centroid deviation and frame
// decode success for BOTH detectors (color flood-fill and distance transform),
// across message sizes, ECC on/off and render scales.
//
// Detection replicates decode_image.cpp; grid snapping uses direct scale with
// floor() (export-image convention).

#include <particle_codec/codec.h>
#include <particle_codec/frame_parser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <queue>
#include <random>
#include <string>
#include <vector>

#define NOMINMAX
#include <windows.h>

using namespace particle_codec;

struct Pixel {
    unsigned char r, g, b, a;
};

static bool isCyan(const Pixel &p) {
    return p.r < 80 && p.g > 100 && p.b > 100 && p.b > p.r + 20 && p.g > p.r + 20;
}

static void renderFrame(const std::vector<std::pair<double, double> > &pts,
                        int gridCols, int gridRows, int scale,
                        std::vector<Pixel> &out, int &w, int &h, double prFactor = 0.6) {
    w = gridCols * scale;
    h = gridRows * scale;
    out.assign((size_t) w * h, Pixel{10, 10, 26, 255});
    int pr = std::max((int) (prFactor * scale), 2);
    int gr = pr * 2;
    for (auto [px, py]: pts) {
        double sx = px * scale, sy = py * scale;
        for (int dy = -gr; dy <= gr; dy++) {
            for (int dx = -gr; dx <= gr; dx++) {
                double d = std::sqrt((double) (dx * dx + dy * dy));
                if (d > gr) continue;
                int x = (int) (sx + dx), y = (int) (sy + dy);
                if (x < 0 || x >= w || y < 0 || y >= h) continue;
                out[(size_t) y * w + x] = Pixel{0, 100, 130, 255};
            }
        }
        for (int dy = -pr; dy <= pr; dy++) {
            for (int dx = -pr; dx <= pr; dx++) {
                double d = std::sqrt((double) (dx * dx + dy * dy));
                if (d > pr) continue;
                int x = (int) (sx + dx), y = (int) (sy + dy);
                if (x < 0 || x >= w || y < 0 || y >= h) continue;
                out[(size_t) y * w + x] = Pixel{0, 188, 212, 255};
            }
        }
    }
}

static std::vector<std::pair<double, double> > detectColor(const std::vector<Pixel> &px, int w, int h) {
    std::vector<bool> visited((size_t) w * h, false);
    std::vector<std::pair<double, double> > out;
    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (visited[idx] || !isCyan(px[idx])) continue;
            double sx = 0, sy = 0;
            int cnt = 0;
            std::queue<std::pair<int, int> > q;
            q.push({x, y});
            visited[idx] = true;
            while (!q.empty()) {
                auto [cx, cy] = q.front();
                q.pop();
                sx += cx;
                sy += cy;
                cnt++;
                for (int d = 0; d < 8; d++) {
                    int nx = cx + dx8[d], ny = cy + dy8[d];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    int nidx = ny * w + nx;
                    if (visited[nidx] || !isCyan(px[nidx])) continue;
                    visited[nidx] = true;
                    q.push({nx, ny});
                }
            }
            if (cnt >= 3) out.push_back({sx / cnt, sy / cnt});
        }
    }
    return out;
}

static std::vector<std::pair<double, double> > detectDT(const std::vector<Pixel> &px, int w, int h,
                                                        int gridCols, int gridRows) {
    int total = w * h;
    std::vector<bool> mask(total);
    int cnt = 0;
    for (int i = 0; i < total; i++) {
        mask[i] = isCyan(px[i]);
        if (mask[i]) cnt++;
    }
    if (cnt < 10) return {};
    std::vector<double> dt(total, 0.0);
    const double INF = 1e9;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            if (!mask[i]) continue;
            double best = INF;
            if (y > 0 && x > 0) best = std::min(best, dt[(y - 1) * w + (x - 1)] + 1.414);
            if (y > 0) best = std::min(best, dt[(y - 1) * w + x] + 1.0);
            if (y > 0 && x < w - 1) best = std::min(best, dt[(y - 1) * w + (x + 1)] + 1.414);
            if (x > 0) best = std::min(best, dt[y * w + (x - 1)] + 1.0);
            dt[i] = (best < INF) ? best : 1.0;
        }
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) {
            int i = y * w + x;
            if (!mask[i]) continue;
            if (y < h - 1 && x > 0) dt[i] = std::min(dt[i], dt[(y + 1) * w + (x - 1)] + 1.414);
            if (y < h - 1) dt[i] = std::min(dt[i], dt[(y + 1) * w + x] + 1.0);
            if (y < h - 1 && x < w - 1) dt[i] = std::min(dt[i], dt[(y + 1) * w + (x + 1)] + 1.414);
            if (x < w - 1) dt[i] = std::min(dt[i], dt[y * w + (x + 1)] + 1.0);
        }
    }
    struct Peak {
        int x, y;
        double v;
    };
    std::vector<Peak> peaks;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int i = y * w + x;
            double v = dt[i];
            if (v < 1.5) continue;
            bool isMax = true;
            for (int dy = -1; dy <= 1 && isMax; dy++)
                for (int dx = -1; dx <= 1 && isMax; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (dt[(y + dy) * w + (x + dx)] >= v) isMax = false;
                }
            if (isMax) peaks.push_back({x, y, v});
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak &a, const Peak &b) { return a.v > b.v; });
    double estX = (double) w / gridCols, estY = (double) h / gridRows;
    double minSpacing = std::min(estX, estY) * 0.4;
    std::vector<std::pair<double, double> > res;
    for (auto &p: peaks) {
        bool tooClose = false;
        for (auto &q: res) {
            double dx = p.x - q.first, dy = p.y - q.second;
            if (std::sqrt(dx * dx + dy * dy) < minSpacing) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) res.push_back({(double) p.x, (double) p.y});
    }
    return res;
}

// Evaluate one detection result: unique snapped cells, per-centroid deviation.
struct SnapResult {
    std::vector<std::pair<int, int> > cells;
    double errSqSum = 0; // sum of per-centroid squared deviation (px^2)
    int errCnt = 0;
    double maxErr = 0;
};

static SnapResult snapCells(const std::vector<std::pair<double, double> > &cents,
                            int gc, int gr, int w, int h) {
    double sx = (double) w / gc, sy = (double) h / gr;
    std::vector<bool> used((size_t) gc * gr, false);
    SnapResult r;
    for (auto [cx, cy]: cents) {
        double gx = cx / sx, gy = cy / sy;
        int col = std::clamp((int) std::floor(gx), 0, gc - 1);
        int row = std::clamp((int) std::floor(gy), 0, gr - 1);
        double ix = (col + 0.5) * sx, iy = (row + 0.5) * sy;
        double e = std::sqrt((cx - ix) * (cx - ix) + (cy - iy) * (cy - iy));
        r.errSqSum += e * e;
        r.errCnt++;
        if (e > r.maxErr) r.maxErr = e;
        int idx = row * gc + col;
        if (!used[idx]) {
            used[idx] = true;
            r.cells.push_back({col, row});
        }
    }
    return r;
}

static void toGridPoints(const std::vector<std::pair<int, int> > &cells,
                         std::vector<std::pair<double, double> > &pts) {
    pts.clear();
    for (auto [c, r]: cells) pts.push_back({c + 0.5, r + 0.5});
}

static bool saveBmp(const char *path, int w, int h, const std::vector<Pixel> &px) {
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bfh.bfType = 0x4D42;
    bfh.bfSize = 54 + w * h * 3;
    bfh.bfOffBits = 54;
    bih.biSize = 40;
    bih.biWidth = w;
    bih.biHeight = -h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    std::vector<unsigned char> bmp(54 + w * h * 3);
    memcpy(bmp.data(), &bfh, 14);
    memcpy(bmp.data() + 14, &bih, 40);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int si = y * w + x, di = 54 + (y * w + x) * 3;
            bmp[di] = px[si].b;
            bmp[di + 1] = px[si].g;
            bmp[di + 2] = px[si].r;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);
    return true;
}

struct Track {
    long frames = 0;
    long trueCells = 0, detectedCells = 0, matchedCells = 0;
    long zeroMissFrames = 0;
    long decodedOk = 0, crcOk = 0;
    double errSqSum = 0;
    long errCnt = 0;
    double maxErr = 0;
};

static void evalTrack(Track &tr, const std::vector<std::pair<int, int> > &trueCells,
                      const SnapResult &snap, ParticleCodec &codec,
                      const std::vector<uint8_t> &stream, int frameIndex) {
    tr.frames++;
    tr.trueCells += (long) trueCells.size();
    tr.detectedCells += (long) snap.cells.size();
    long matched = 0;
    for (auto &c: trueCells)
        for (auto &d: snap.cells)
            if (c == d) {
                matched++;
                break;
            }
    tr.matchedCells += matched;
    if (matched == (long) trueCells.size() && (long) snap.cells.size() == (long) trueCells.size())
        tr.zeroMissFrames++;
    if (snap.errCnt > 0) {
        tr.errSqSum += snap.errSqSum;
        tr.errCnt += snap.errCnt;
        if (snap.maxErr > tr.maxErr) tr.maxErr = snap.maxErr;
    }
    std::vector<std::pair<double, double> > gpts;
    toGridPoints(snap.cells, gpts);
    auto decoded = codec.decodeCentroids(gpts);
    if (decoded && !decoded->empty()) {
        auto hdr = FrameHeader::tryParse(*decoded);
        if (hdr) {
            int off = FrameHeader::totalSize;
            int len = hdr->payloadLength;
            if (off + len <= (int) decoded->size()) {
                tr.crcOk++;
                int chunkSize = codec.maxPayloadBytes();
                int begin = frameIndex * chunkSize;
                bool match = true;
                for (int k = 0; k < len; k++) {
                    int src = begin + k;
                    if (src >= (int) stream.size()) {
                        match = false;
                        break;
                    }
                    if ((*decoded)[off + k] != stream[src]) {
                        match = false;
                        break;
                    }
                }
                if (match) tr.decodedOk++;
            }
        }
    }
}

static void printTrack(const std::string &name, const Track &tr) {
    double recall = tr.trueCells ? 100.0 * tr.matchedCells / tr.trueCells : 0.0;
    double prec = tr.detectedCells ? 100.0 * tr.matchedCells / tr.detectedCells : 0.0;
    double rms = tr.errCnt ? std::sqrt(tr.errSqSum / tr.errCnt) : 0.0;
    std::cout << "  " << std::left << std::setw(12) << name
            << " recall=" << std::fixed << std::setprecision(2) << recall << "%"
            << " prec=" << prec << "%"
            << " zeroMiss=" << tr.zeroMissFrames << "/" << tr.frames
            << " crc=" << tr.crcOk << "/" << tr.frames
            << " decode=" << tr.decodedOk << "/" << tr.frames
            << " rms=" << rms << "px max=" << std::setprecision(2) << tr.maxErr << "px"
            << std::setprecision(6) << "\n";
}

static void runSeries(const std::string &label, const std::vector<int> &sizes, bool ecc,
                      int gridCols, int gridRows, int scale, int trials,
                      Track &tColor, Track &tDT, Track &tBest,
                      bool saveSample, const std::string &samplePath, double prFactor = 0.6) {
    std::mt19937 rng(12345);
    bool sampleSaved = false;
    for (int t = 0; t < trials; t++) {
        for (int size: sizes) {
            std::vector<uint8_t> data(size);
            for (auto &b: data) b = (uint8_t)(rng() & 0xFF);
            std::vector<uint8_t> stream = ecc ? HammingEncoder::encode(data) : data;
            ParticleCodec codec("particle_codec", gridCols, gridRows);
            auto frames = ecc ? codec.encodeWithEcc(data) : codec.encode(data);
            for (size_t fi = 0; fi < frames.size(); fi++) {
                auto &frame = frames[fi];
                std::vector<bool> trueUsed((size_t) gridCols * gridRows, false);
                std::vector<std::pair<int, int> > trueCells;
                for (int i = 0; i < frame.particleCount; i++) {
                    int col = (int) std::floor(frame.particles[i * 2]);
                    int row = (int) std::floor(frame.particles[i * 2 + 1]);
                    int idx = row * gridCols + col;
                    if (!trueUsed[idx]) {
                        trueUsed[idx] = true;
                        trueCells.push_back({col, row});
                    }
                }
                std::vector<std::pair<double, double> > pts;
                for (int i = 0; i < frame.particleCount; i++)
                    pts.push_back({frame.particles[i * 2], frame.particles[i * 2 + 1]});

                std::vector<Pixel> px;
                int w, h;
                renderFrame(pts, gridCols, gridRows, scale, px, w, h, prFactor);
                if (saveSample && !sampleSaved) {
                    if (saveBmp(samplePath.c_str(), w, h, px)) sampleSaved = true;
                }

                auto cColor = detectColor(px, w, h);
                auto cDT = detectDT(px, w, h, gridCols, gridRows);
                auto sColor = snapCells(cColor, gridCols, gridRows, w, h);
                auto sDT = snapCells(cDT, gridCols, gridRows, w, h);
                evalTrack(tColor, trueCells, sColor, codec, stream, (int) fi);
                evalTrack(tDT, trueCells, sDT, codec, stream, (int) fi);
                // decode_image style fallback: color first, DT second
                const SnapResult *best = sColor.cells.size() >= sDT.cells.size() ? &sColor : &sDT;
                evalTrack(tBest, trueCells, *best, codec, stream, (int) fi);
            }
        }
    }
    std::cout << std::left << std::setw(30) << label << " (" << tColor.frames << " frames)\n";
    printTrack("color", tColor);
    printTrack("dt", tDT);
    printTrack("best", tBest);
}

int main() {
    std::cout << "=== Particle Codec Precision Benchmark ===\n\n";
    const std::vector<int> sizes60 = {1, 32, 128, 300, 438};
    const int trials60 = 6;

    Track a1c, a1d, a1b, a2c, a2d, a2b, a3c, a3d, a3b, a4c, a4d, a4b, a5c, a5d, a5b;
    std::cout << "[60x60 grid, viewer export scale=8] sizes {1,32,128,300,438}, 6 trials\n";
    runSeries("no-ECC, scale=8", sizes60, false, 60, 60, 8, trials60, a1c, a1d, a1b, true,
              "precision_sample.bmp");
    runSeries("ECC, scale=8", sizes60, true, 60, 60, 8, trials60, a2c, a2d, a2b, false, "");
    std::cout << "\n[robustness at other scales, 128-byte message, 10 trials]\n";
    runSeries("no-ECC, scale=4", {128}, false, 60, 60, 4, 10, a3c, a3d, a3b, false, "");
    runSeries("no-ECC, scale=16", {128}, false, 60, 60, 16, 10, a4c, a4d, a4b, false, "");
    std::cout << "\n[30x30 grid, scale=8, sizes {1,64,96}, 6 trials]\n";
    Track a5d2, a5b2;
    runSeries("no-ECC, 30x30", {1, 64, 96}, false, 30, 30, 8, 6, a5c, a5d2, a5b2, false, "");
    std::cout << "\n[120x120 grid, scale=4 render 480x480, sizes {1,64,100}, 3 trials]\n";
    Track a6c, a6d, a6b;
    runSeries("no-ECC, 120x120/4", {1, 64, 100}, false, 120, 120, 4, 3, a6c, a6d, a6b, false, "");
    std::cout << "\n[60x60 scale=8, smaller core pr=0.35*scale=2, dense sizes {300,438}, 8 trials]\n";
    Track a7c, a7d, a7b;
    runSeries("no-ECC, pr=0.35", {300, 438}, false, 60, 60, 8, 8, a7c, a7d, a7b, false, "", 0.35);
    std::cout << "\nDone. Sample image: precision_sample.bmp\n";
    return 0;
}
