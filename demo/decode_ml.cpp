// Hybrid ML decoder: geometry is recovered by the classical GridCalibrator,
// then the trained CNN detects the 60x60 particle bitmap on the rectified
// image, and the standard codec path decodes the payload.
//
// Usage: decode_ml.exe <image> [weights.bin] [grid_cols] [grid_rows]
// Exit codes: 0=decoded, 1=usage/argument error, 2=I/O error,
//             3=decode failed, 4=internal error
#include <particle_codec/codec.h>
#include <particle_codec/frame_builder.h>
#include <particle_codec/frame_parser.h>
#include <particle_codec/grid_calibrator.h>
#include <particle_codec/particle_net.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace particle_codec;

struct Contour {
    double cx, cy;
    int area;
};

static inline bool is_cyan(unsigned char r, unsigned char g, unsigned char b) {
    return r < 80 && g > 100 && b > 100 && b > r + 20 && g > r + 20;
}

// Intensity-weighted color flood-fill (same as decode_image.cpp).
static std::vector<Contour> detect_by_color(std::vector<uint8_t> &mask,
                                            int w, int h,
                                            const std::vector<float> *weights) {
    std::vector<Contour> contours;
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
                contours.push_back({sumX / sumW, sumY / sumW, count});
        }
    }
    return contours;
}

// Distance transform + local maxima: separates overlapping glow blobs into
// individual particle centers (the color flood-fill merges them when the
// glow radius exceeds half the grid spacing, e.g. the 1080 export).
static std::vector<Contour> detect_by_distance_transform(
    const std::vector<uint8_t> &mask, int w, int h, int gridCols,
    int gridRows) {
    int total = w * h;
    int count = 0;
    for (int i = 0; i < total; i++) count += mask[i];
    if (count < 10) return {};

    std::vector<float> dt(total, 0.0f);
    const float INF = 1e9f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            if (!mask[i]) continue;
            float best = INF;
            if (y > 0 && x > 0) best = std::min(best, dt[(y - 1) * w + (x - 1)] + 1.414f);
            if (y > 0) best = std::min(best, dt[(y - 1) * w + x] + 1.0f);
            if (y > 0 && x < w - 1) best = std::min(best, dt[(y - 1) * w + (x + 1)] + 1.414f);
            if (x > 0) best = std::min(best, dt[y * w + (x - 1)] + 1.0f);
            dt[i] = (best < INF) ? best : 1.0f;
        }
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) {
            int i = y * w + x;
            if (!mask[i]) continue;
            if (y < h - 1 && x > 0) dt[i] = std::min(dt[i], dt[(y + 1) * w + (x - 1)] + 1.414f);
            if (y < h - 1) dt[i] = std::min(dt[i], dt[(y + 1) * w + x] + 1.0f);
            if (y < h - 1 && x < w - 1) dt[i] = std::min(dt[i], dt[(y + 1) * w + (x + 1)] + 1.414f);
            if (x < w - 1) dt[i] = std::min(dt[i], dt[y * w + (x + 1)] + 1.0f);
        }
    }

    struct Peak {
        int x, y;
        float val;
    };
    std::vector<Peak> peaks;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int i = y * w + x;
            float v = dt[i];
            if (v < 1.5f) continue;
            bool isMax = true;
            for (int dy = -1; dy <= 1 && isMax; dy++)
                for (int dx = -1; dx <= 1 && isMax; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (dt[(y + dy) * w + (x + dx)] >= v) isMax = false;
                }
            if (isMax) peaks.push_back({x, y, v});
        }
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const Peak &a, const Peak &b) { return a.val > b.val; });

    double estSpacingX = static_cast<double>(w) / gridCols;
    double estSpacingY = static_cast<double>(h) / gridRows;
    double minSpacing = std::min(estSpacingX, estSpacingY) * 0.4;
    std::vector<Contour> result;
    for (const auto &p : peaks) {
        bool tooClose = false;
        for (const auto &q : result) {
            double dx = p.x - q.cx;
            double dy = p.y - q.cy;
            if (std::sqrt(dx * dx + dy * dy) < minSpacing) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) result.push_back({static_cast<double>(p.x),
                                         static_cast<double>(p.y), 1});
    }
    return result;
}

// Warp the source image into the canonical 240x240 model input.
// `aff` maps source pixels -> canonical grid coordinates (col+0.5,row+0.5).
// Training feeds the CNN images produced by F.interpolate(480->240,
// bilinear, align_corners=False), whose output pixel X samples source pixel
// 2X + 0.5 (i.e. grid g = (2X + 0.5)/8 = X/4 + 0.0625). The warp must use
// that same half-source-pixel convention or cells near particle edges flip.
// Bilinear sampling, black outside the image.
static bool warpToModel(const unsigned char *rgb, int w, int h,
                        const GridCalibrator::Affine &aff,
                        float *out240) {
    double det = aff.a * aff.e - aff.b * aff.d;
    if (std::abs(det) < 1e-9) return false;
    double ia = aff.e / det, ib = -aff.b / det;
    double id = -aff.d / det, ie = aff.a / det;
    double ic = (aff.b * aff.f - aff.e * aff.c) / det;
    double jc = (aff.d * aff.c - aff.a * aff.f) / det;

    const int S = ParticleNet::kInputSize;
    for (int y = 0; y < S; y++) {
        double gy = y / 4.0 + 0.0625;
        for (int x = 0; x < S; x++) {
            double gx = x / 4.0 + 0.0625;
            double px = ia * gx + ib * gy + ic;
            double py = id * gx + ie * gy + jc;
            float *dst = out240 + (static_cast<size_t>(y) * S + x) * 3;
            if (px < 0 || px > w - 1 || py < 0 || py > h - 1) {
                dst[0] = dst[1] = dst[2] = 0.0f;
                continue;
            }
            int x0 = static_cast<int>(std::floor(px));
            int y0 = static_cast<int>(std::floor(py));
            int x1 = std::min(x0 + 1, w - 1);
            int y1 = std::min(y0 + 1, h - 1);
            double fx = px - x0, fy = py - y0;
            for (int c = 0; c < 3; c++) {
                double v =
                    (1.0 - fy) *
                        ((1.0 - fx) * rgb[(y0 * w + x0) * 3 + c] +
                         fx * rgb[(y0 * w + x1) * 3 + c]) +
                    fy * ((1.0 - fx) * rgb[(y1 * w + x0) * 3 + c] +
                          fx * rgb[(y1 * w + x1) * 3 + c]);
                dst[c] = static_cast<float>(v / 255.0);
            }
        }
    }
    return true;
}

struct MlAttempt {
    std::string label;
    std::string error;
    bool succeeded = false;
    std::string payload;
    int cells = 0;
};

static MlAttempt ml_decode(const unsigned char *rgb, int w, int h,
                           const GridCalibrator::Affine &aff,
                           const ParticleNet &net, ParticleCodec &codec,
                           int gridCols, int gridRows,
                           const std::string &label) {
    MlAttempt r;
    r.label = label;
    std::vector<float> model(static_cast<size_t>(ParticleNet::kInputSize) *
                             ParticleNet::kInputSize * 3);
    if (!warpToModel(rgb, w, h, aff, model.data())) {
        r.error = "bad affine";
        return r;
    }

    std::vector<float> logits(static_cast<size_t>(ParticleNet::kGrid) *
                              ParticleNet::kGrid);
    net.detect(model.data(), logits.data());

    std::vector<std::pair<double, double> > centroids;
    for (int row = 0; row < gridRows; row++) {
        for (int col = 0; col < gridCols; col++) {
            float p = 1.0f / (1.0f + std::exp(-logits[row * gridCols + col]));
            if (p >= 0.5f) centroids.emplace_back(col + 0.5, row + 0.5);
        }
    }
    r.cells = static_cast<int>(centroids.size());

    auto res = codec.decodeCentroidsDetailed(centroids);
    auto emit_payload = [&](const std::vector<uint8_t> &bytes) {
        auto header = FrameHeader::tryParse(bytes);
        if (!header || header->payloadLength <= 0) return false;
        r.succeeded = true;
        r.payload.assign(bytes.begin() + FrameHeader::totalSize,
                         bytes.begin() + FrameHeader::totalSize +
                             header->payloadLength);
        return true;
    };
    if (res.ok()) {
        emit_payload(res.value());
        return r;
    }
    r.error = errorName(res.error().code);

    // CRC-guided bit repair: the bitmap is usually off by 1-2 bits only.
    if (res.error().code == ErrorCode::CrcMismatch) {
        auto raw = codec.decodeCentroidsRaw(centroids);
        if (raw && raw->size() >= 5) {
            const size_t nbits = raw->size() * 8;
            auto flip1 = [&](size_t i) {
                std::vector<uint8_t> f = *raw;
                f[i / 8] ^= static_cast<uint8_t>(1 << (7 - (i % 8)));
                return f;
            };
            for (size_t i = 0; i < nbits; i++)
                if (FrameBuilder::verifyCrc(flip1(i))) {
                    if (emit_payload(flip1(i))) return r;
                }
            for (size_t i = 0; i < nbits; i++) {
                std::vector<uint8_t> f = flip1(i);
                for (size_t j = i + 1; j < nbits; j++) {
                    f[j / 8] ^= static_cast<uint8_t>(1 << (7 - (j % 8)));
                    if (FrameBuilder::verifyCrc(f)) {
                        if (emit_payload(f)) return r;
                    }
                    f[j / 8] ^= static_cast<uint8_t>(1 << (7 - (j % 8)));
                }
            }
        }
    }
    return r;
}

static std::string exe_dir(const char *argv0) {
    std::string p = argv0;
    auto pos = p.find_last_of("/\\");
    return (pos == std::string::npos) ? std::string() : p.substr(0, pos + 1);
}

static bool file_exists(const std::string &path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
#endif
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: decode_ml <image> [weights.bin] [grid_cols] [grid_rows]\n";
        return 1;
    }
    std::string imagePath = argv[1];
    std::string weightsPath =
        (argc > 2) ? argv[2] : "particle_detector.bin";
    int gridCols = (argc > 3) ? std::atoi(argv[3]) : 60;
    int gridRows = (argc > 4) ? std::atoi(argv[4]) : 60;

    if (!file_exists(imagePath)) {
        std::cerr << "Error: file not found: " << imagePath << std::endl;
        return 1;
    }
    if (!file_exists(weightsPath)) {
        std::string alt = exe_dir(argv[0]) + weightsPath;
        if (file_exists(alt)) weightsPath = alt;
    }

    ParticleNet net;
    if (!net.load(weightsPath)) {
        std::cerr << "Error: cannot load weights: " << weightsPath
                  << " (run train/export_net.py first)" << std::endl;
        return 2;
    }

    int width = 0, height = 0, channels = 0;
    unsigned char *img = stbi_load(imagePath.c_str(), &width, &height,
                                   &channels, 3);
    if (!img) {
        std::cerr << "Error: failed to load image: " << imagePath << std::endl;
        return 2;
    }

    // Geometry from the classical pipeline: color detection -> calibrator.
    int total = width * height;
    std::vector<uint8_t> mask(total, 0);
    std::vector<float> weights(total, 0);
    for (int i = 0; i < total; i++)
        if (is_cyan(img[i * 3], img[i * 3 + 1], img[i * 3 + 2])) {
            mask[i] = 1;
            weights[i] = static_cast<float>(img[i * 3 + 1] + img[i * 3 + 2]);
        }
    std::vector<uint8_t> maskForDT = mask;  // color detection consumes mask
    auto contours = detect_by_color(mask, width, height, &weights);

    std::vector<std::pair<double, double> > colorCentroids;
    for (const auto &c : contours) colorCentroids.emplace_back(c.cx, c.cy);
    std::vector<std::pair<double, double> > dtCentroids;
    for (const auto &c : detect_by_distance_transform(maskForDT, width, height,
                                                      gridCols, gridRows))
        dtCentroids.emplace_back(c.cx, c.cy);
    std::cout << "Image: " << width << "x" << height
              << ", color particles: " << colorCentroids.size()
              << ", dt particles: " << dtCentroids.size() << std::endl;

    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto try_centroids = [&](const std::vector<std::pair<double, double> > &pts,
                             const char *tag) -> bool {
        std::vector<GridCalibrator::Affine> variants;
        auto cal = GridCalibrator::calibrate(pts, gridCols, gridRows);
        if (cal.valid) {
            variants = {cal, cal.rotated(), cal.rotated().rotated(),
                        cal.rotated().rotated().rotated()};
        } else {
            std::cout << "  [" << tag << "] calibration failed; "
                         "axis-aligned fallback" << std::endl;
            GridCalibrator::Affine direct;
            direct.a = static_cast<double>(gridCols) / width;
            direct.e = static_cast<double>(gridRows) / height;
            direct.valid = true;
            variants = {direct};
        }
        for (size_t v = 0; v < variants.size(); v++) {
            // GridCalibrator's translation is arbitrary: align so the
            // canonical 240 image covers cells [0, 60) (map_and_align).
            GridCalibrator::Affine aligned = variants[v];
            if (!pts.empty()) {
                double minX = 1e18, minY = 1e18;
                for (const auto &c : pts) {
                    auto m = aligned.map(c.first, c.second);
                    minX = std::min(minX, m.first);
                    minY = std::min(minY, m.second);
                }
                aligned.c -= std::round(minX - 0.5);
                aligned.f -= std::round(minY - 0.5);
            }
            std::string label = std::string(tag) + (cal.valid
                ? " calibrated (orientation " + std::to_string(v) + ")"
                : " direct");
            auto r = ml_decode(img, width, height, aligned, net, codec,
                               gridCols, gridRows, label);
            std::cout << "  [" << r.label << "] cells=" << r.cells;
            if (r.succeeded) {
                std::cout << " OK" << std::endl;
#ifdef _WIN32
                // Payload is binary: disable CRLF translation.
                _setmode(_fileno(stdout), _O_BINARY);
#endif
                std::cout << r.payload << std::flush;
                return true;
            }
            std::cout << " -> " << r.error << std::endl;
        }
        return false;
    };

    if (try_centroids(colorCentroids, "ml color"))
        { stbi_image_free(img); return 0; }
    if (try_centroids(dtCentroids, "ml dt"))
        { stbi_image_free(img); return 0; }
    stbi_image_free(img);
    std::cerr << "Decode failed. All geometry variants exhausted."
              << std::endl;
    return 3;
}
