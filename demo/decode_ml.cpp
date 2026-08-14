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
#include <particle_codec/homography_calibrator.h>
#include <particle_codec/particle_net.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// Core-biased mask: only the bright particle core (0,188,212) passes, not
// the dim glow (0,100,130). When glow radius exceeds half the grid spacing
// (or blur/glare smears particles), the glow mask merges neighbouring
// particles into big blobs; the core mask keeps them separable for the
// distance transform.
static inline bool is_cyan_core(unsigned char r, unsigned char g,
                                unsigned char b) {
    return r < 90 && g > 150 && b > 150 && b > r + 40 && g > r + 40;
}

// Intensity-weighted color flood-fill (same as decode_image.cpp).
static std::vector<Contour> detect_by_color(std::vector<uint8_t> &mask,
                                            int w, int h,
                                            const std::vector<float> *weights) {
    std::vector<Contour> contours;
    std::vector<int> cMinX, cMaxX, cMinY, cMaxY, cMaxW;
    std::vector<int> stack;
    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (mask[idx] == 0) continue;
            double sumX = 0, sumY = 0, sumW = 0;
            int count = 0;
            int minX = x, maxX = x, minY = y, maxY = y;
            int maxW = 0;
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
                int wti = static_cast<int>(wt);
                if (wti > maxW) maxW = wti;
                minX = std::min(minX, cx);
                maxX = std::max(maxX, cx);
                minY = std::min(minY, cy);
                maxY = std::max(maxY, cy);
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
            if (count >= 3 && sumW > 0) {
                contours.push_back({sumX / sumW, sumY / sumW, count});
                cMinX.push_back(minX);
                cMaxX.push_back(maxX);
                cMinY.push_back(minY);
                cMaxY.push_back(maxY);
                cMaxW.push_back(maxW);
            }
        }
    }
    // Robust filtering: keep only compact, round blobs near the dominant
    // particle size. Fragments (tiny noise) and merged blobs (glow/glare
    // clusters) are rejected so calibration gets clean centroids.
    if (contours.size() >= 6) {
        std::vector<int> areas;
        for (const auto &c : contours) areas.push_back(c.area);
        std::sort(areas.begin(), areas.end());
        int med = areas[areas.size() / 2];
        std::vector<Contour> kept;
        for (size_t i = 0; i < contours.size(); i++) {
            int bw = cMaxX[i] - cMinX[i] + 1;
            int bh = cMaxY[i] - cMinY[i] + 1;
            double roundness =
                static_cast<double>(contours[i].area) / std::max(bw * bh, 1);
            if (contours[i].area < std::max(3, med / 4)) continue;
            if (contours[i].area > med * 5) continue;
            if (roundness < 0.25) continue;
            if (cMaxW[i] < 200) continue;  // must contain a bright core
            kept.push_back(contours[i]);
        }
        if (kept.size() >= 6) contours = kept;
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

// Warp the source image into the canonical 240x240 model input through the
// inverse of an image->grid transform (affine or homography).
// Training feeds the CNN images produced by F.interpolate(480->240,
// bilinear, align_corners=False), whose output pixel X samples source pixel
// 2X + 0.5 (i.e. grid g = (2X + 0.5)/8 = X/4 + 0.0625). The warp must use
// that same half-source-pixel convention or cells near particle edges flip.
// Bilinear sampling, black outside the image.
static void warpToModelInv(const unsigned char *rgb, int w, int h,
                           const double inv[3][3], float *out240) {
    const int S = ParticleNet::kInputSize;
    for (int y = 0; y < S; y++) {
        double gy = y / 4.0 + 0.0625;
        for (int x = 0; x < S; x++) {
            double gx = x / 4.0 + 0.0625;
            double wgt = inv[2][0] * gx + inv[2][1] * gy + inv[2][2];
            if (std::abs(wgt) < 1e-12) wgt = 1.0;
            double px = (inv[0][0] * gx + inv[0][1] * gy + inv[0][2]) / wgt;
            double py = (inv[1][0] * gx + inv[1][1] * gy + inv[1][2]) / wgt;
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
}

// Inverse of an affine image->grid transform as a 3x3 matrix.
static bool affineInverse(const GridCalibrator::Affine &aff, double inv[3][3]) {
    double det = aff.a * aff.e - aff.b * aff.d;
    if (std::abs(det) < 1e-9) return false;
    double ia = aff.e / det, ib = -aff.b / det;
    double id = -aff.d / det, ie = aff.a / det;
    double ic = (aff.b * aff.f - aff.e * aff.c) / det;
    double jc = (aff.d * aff.c - aff.a * aff.f) / det;
    inv[0][0] = ia; inv[0][1] = ib; inv[0][2] = ic;
    inv[1][0] = id; inv[1][1] = ie; inv[1][2] = jc;
    inv[2][0] = 0; inv[2][1] = 0; inv[2][2] = 1;
    return true;
}

// Inverse of a homography (adjugate / determinant).
static bool homographyInverse(const Homography &h, double inv[3][3]) {
    double a = h.h[0][0], b = h.h[0][1], c = h.h[0][2];
    double d = h.h[1][0], e = h.h[1][1], f = h.h[1][2];
    double g = h.h[2][0], k = h.h[2][1], m = h.h[2][2];
    double det = a * (e * m - f * k) - b * (d * m - f * g) + c * (d * k - e * g);
    if (std::abs(det) < 1e-12) return false;
    inv[0][0] = (e * m - f * k) / det;
    inv[0][1] = (c * k - b * m) / det;
    inv[0][2] = (b * f - c * e) / det;
    inv[1][0] = (f * g - d * m) / det;
    inv[1][1] = (a * m - c * g) / det;
    inv[1][2] = (c * d - a * f) / det;
    inv[2][0] = (d * k - e * g) / det;
    inv[2][1] = (b * g - a * k) / det;
    inv[2][2] = (a * e - b * d) / det;
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
                           const double inv[3][3],
                           const ParticleNet &net, ParticleCodec &codec,
                           int gridCols, int gridRows,
                           const std::string &label) {
    MlAttempt r;
    r.label = label;
    std::vector<float> model(static_cast<size_t>(ParticleNet::kInputSize) *
                             ParticleNet::kInputSize * 3);
    warpToModelInv(rgb, w, h, inv, model.data());

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
    std::vector<uint8_t> mask(total, 0), coreMask(total, 0);
    std::vector<float> weights(total, 0);
    for (int i = 0; i < total; i++) {
        unsigned char r = img[i * 3], g = img[i * 3 + 1], b = img[i * 3 + 2];
        if (is_cyan(r, g, b)) {
            mask[i] = 1;
            weights[i] = static_cast<float>(g + b);
        }
        if (is_cyan_core(r, g, b)) coreMask[i] = 1;
    }
    auto contours = detect_by_color(mask, width, height, &weights);

    std::vector<std::pair<double, double> > colorCentroids;
    for (const auto &c : contours) colorCentroids.emplace_back(c.cx, c.cy);
    std::vector<std::pair<double, double> > dtCentroids;
    for (const auto &c : detect_by_distance_transform(coreMask, width, height,
                                                      gridCols, gridRows))
        dtCentroids.emplace_back(c.cx, c.cy);
    std::cout << "Image: " << width << "x" << height
              << ", color particles: " << colorCentroids.size()
              << ", dt particles: " << dtCentroids.size() << std::endl;

    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto run_and_report = [&](const std::string &label,
                              const double inv[3][3]) -> bool {
        auto r = ml_decode(img, width, height, inv, net, codec, gridCols,
                           gridRows, label);
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
        return false;
    };

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
            double inv[3][3];
            if (!affineInverse(aligned, inv)) continue;
            if (run_and_report(label, inv)) return true;
        }
        return false;
    };

    // Perspective fallback: fit a homography (8-DOF) from the detected
    // centroids, which handles keystone distortion from angled photos.
    auto try_homography = [&](const std::vector<std::pair<double, double> > &pts,
                              const char *tag) -> bool {
        Homography hs[HomographyCalibrator::kMaxVariants];
        int n = HomographyCalibrator::calibrate(pts, gridCols, gridRows, hs);
        if (n <= 0) {
            std::cout << "  [" << tag << "] homography calibration failed"
                      << std::endl;
            return false;
        }
        for (int k = 0; k < n; k++) {
            double inv[3][3];
            if (!homographyInverse(hs[k], inv)) continue;
            std::string label = std::string(tag) +
                                " homography variant " + std::to_string(k);
            if (run_and_report(label, inv)) return true;
        }
        return false;
    };

    if (try_centroids(colorCentroids, "ml color"))
        { stbi_image_free(img); return 0; }
    if (try_centroids(dtCentroids, "ml dt"))
        { stbi_image_free(img); return 0; }
    if (try_homography(dtCentroids, "ml dt"))
        { stbi_image_free(img); return 0; }
    if (try_homography(colorCentroids, "ml color"))
        { stbi_image_free(img); return 0; }
    stbi_image_free(img);
    std::cerr << "Decode failed. All geometry variants exhausted."
              << std::endl;
    return 3;
}
