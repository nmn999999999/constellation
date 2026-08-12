#include <particle_codec/codec.h>
#include <particle_codec/frame_parser.h>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace particle_codec;

struct Contour {
    double cx, cy;
    int area;
};

// Path existence check that never throws. On Windows the argv / fopen stack
// works in the ANSI code page (GBK on Chinese systems), so GetFileAttributesA
// is used instead of std::filesystem::exists, whose narrow->wide conversion
// throws on non-ASCII paths (e.g. E:\私有粒子\...).
static bool file_exists(const std::string &path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
#endif
}

static inline bool is_cyan(unsigned char r, unsigned char g, unsigned char b) {
    return r < 80 && g > 100 && b > 100 && b > r + 20 && g > r + 20;
}

// Load an image and reduce it to a 1 byte/pixel foreground mask immediately,
// so the full RGBA buffer (4 bytes/pixel) is freed before detection starts.
static std::vector<uint8_t> load_cyan_mask(const std::string &path, int &width, int &height) {
    int channels;
    unsigned char *data = stbi_load(path.c_str(), &width, &height, &channels, 3);
    if (!data) return {};

    int total = width * height;
    std::vector<uint8_t> mask(total, 0);
    for (int i = 0; i < total; i++) {
        mask[i] = is_cyan(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]) ? 1 : 0;
    }
    stbi_image_free(data);
    return mask;
}

static std::vector<std::pair<double, double> > contours_to_grid(
    const std::vector<Contour> &contours,
    int gridCols, int gridRows, int imageWidth, int imageHeight) {
    double scaleX = static_cast<double>(imageWidth) / gridCols;
    double scaleY = static_cast<double>(imageHeight) / gridRows;
    double scale = std::min(scaleX, scaleY);
    double ox = (imageWidth - gridCols * scale) * 0.5;
    double oy = (imageHeight - gridRows * scale) * 0.5;

    std::vector<std::pair<double, double> > gridPoints;
    std::vector<bool> cellUsed(gridCols * gridRows, false);

    for (auto &c: contours) {
        double gx = (c.cx - ox) / scale;
        double gy = (c.cy - oy) / scale;
        int col = std::clamp(static_cast<int>(std::floor(gx)), 0, gridCols - 1);
        int row = std::clamp(static_cast<int>(std::floor(gy)), 0, gridRows - 1);
        int idx = row * gridCols + col;
        if (!cellUsed[idx]) {
            cellUsed[idx] = true;
            gridPoints.emplace_back(col + 0.5, row + 0.5);
        }
    }
    return gridPoints;
}

static std::optional<std::string> try_decode(
    const std::vector<std::pair<double, double> > &gridPoints,
    int gridCols, int gridRows) {
    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto frameBytes = codec.decodeCentroids(gridPoints);
    if (frameBytes.has_value() && !frameBytes->empty()) {
        auto header = FrameHeader::tryParse(*frameBytes);
        if (header && header->payloadLength > 0) {
            return std::string(
                frameBytes->begin() + FrameHeader::totalSize,
                frameBytes->begin() + FrameHeader::totalSize + header->payloadLength);
        }
    }
    return std::nullopt;
}

// Distance transform. Uses a float buffer (4 bytes/pixel) instead of double.
static std::vector<Contour> detect_by_distance_transform(
    const std::vector<uint8_t> &mask, int w, int h, int gridCols, int gridRows) {
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
    std::sort(peaks.begin(), peaks.end(), [](auto &a, auto &b) { return a.val > b.val; });

    double estSpacingX = static_cast<double>(w) / gridCols;
    double estSpacingY = static_cast<double>(h) / gridRows;
    double minSpacing = std::min(estSpacingX, estSpacingY) * 0.4;

    std::vector<std::pair<double, double> > result;
    for (auto &p: peaks) {
        bool tooClose = false;
        for (auto &q: result) {
            double dx = p.x - q.first;
            double dy = p.y - q.second;
            if (std::sqrt(dx * dx + dy * dy) < minSpacing) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) result.push_back({static_cast<double>(p.x), static_cast<double>(p.y)});
    }

    std::vector<Contour> contours;
    for (auto &[x, y]: result) contours.push_back({x, y, 1});
    return contours;
}

// Color flood-fill. The mask is consumed in place (visited cells are zeroed)
// and a plain vector is used as the stack, avoiding a separate visited buffer
// and per-pixel queue allocations.
static std::vector<Contour> detect_by_color(std::vector<uint8_t> &mask, int w, int h) {
    std::vector<Contour> contours;
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
            if (count >= 3) {
                contours.push_back({sumX / count, sumY / count, count});
            }
        }
    }
    return contours;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: decode_image <image_path> [grid_cols] [grid_rows]" << std::endl;
        return 1;
    }

    std::string imagePath = argv[1];
    int gridCols = (argc > 2) ? std::atoi(argv[2]) : 60;
    int gridRows = (argc > 3) ? std::atoi(argv[3]) : 60;

    if (gridCols <= 0 || gridRows <= 0) {
        std::cerr << "Error: grid_cols/grid_rows must be positive integers" << std::endl;
        return 1;
    }

    if (!file_exists(imagePath)) {
        std::cerr << "Error: file not found: " << imagePath << std::endl;
        return 1;
    }

    int width, height;
    auto mask = load_cyan_mask(imagePath, width, height);
    if (mask.empty()) {
        std::cerr << "Error: failed to load image: " << imagePath << std::endl;
        return 1;
    }

    int maxCount = gridCols * gridRows;

    // Method 1: Color flood-fill (runs on a copy so the mask stays intact for DT)
    {
        std::vector<uint8_t> maskCopy = mask;
        auto colorContours = detect_by_color(maskCopy, width, height);
        auto gp1 = contours_to_grid(colorContours, gridCols, gridRows, width, height);
        auto r1 = try_decode(gp1, gridCols, gridRows);
        if (r1) {
            std::cout << *r1 << std::flush;
            return 0;
        }

        // Method 4: Color + direct scale
        {
            double scaleX = static_cast<double>(width) / gridCols;
            double scaleY = static_cast<double>(height) / gridRows;
            std::vector<std::pair<double, double> > gp4;
            std::vector<bool> cellUsed(maxCount, false);
            for (auto &c: colorContours) {
                double gx = c.cx / scaleX;
                double gy = c.cy / scaleY;
                int col = std::clamp(static_cast<int>(std::floor(gx)), 0, gridCols - 1);
                int row = std::clamp(static_cast<int>(std::floor(gy)), 0, gridRows - 1);
                int idx = row * gridCols + col;
                if (!cellUsed[idx]) {
                    cellUsed[idx] = true;
                    gp4.emplace_back(col + 0.5, row + 0.5);
                }
            }
            auto r4 = try_decode(gp4, gridCols, gridRows);
            if (r4) {
                std::cout << *r4 << std::flush;
                return 0;
            }
        }
    }

    // Method 2: Distance transform
    auto dtContours = detect_by_distance_transform(mask, width, height, gridCols, gridRows);
    auto gp2 = contours_to_grid(dtContours, gridCols, gridRows, width, height);
    auto r2 = try_decode(gp2, gridCols, gridRows);
    if (r2) {
        std::cout << *r2 << std::flush;
        return 0;
    }

    // Mask is no longer needed; release it before the last method.
    std::vector<uint8_t>().swap(mask);

    // Method 3: Direct grid-snapping without centering offset (for export images)
    {
        double scaleX = static_cast<double>(width) / gridCols;
        double scaleY = static_cast<double>(height) / gridRows;
        std::vector<std::pair<double, double> > gp3;
        std::vector<bool> cellUsed(maxCount, false);
        for (auto &c: dtContours) {
            double gx = c.cx / scaleX;
            double gy = c.cy / scaleY;
            int col = std::clamp(static_cast<int>(std::floor(gx)), 0, gridCols - 1);
            int row = std::clamp(static_cast<int>(std::floor(gy)), 0, gridRows - 1);
            int idx = row * gridCols + col;
            if (!cellUsed[idx]) {
                cellUsed[idx] = true;
                gp3.emplace_back(col + 0.5, row + 0.5);
            }
        }
        auto r3 = try_decode(gp3, gridCols, gridRows);
        if (r3) {
            std::cout << *r3 << std::flush;
            return 0;
        }
    }

    std::cerr << "Decode failed. All methods exhausted." << std::endl;
    return 1;
}
