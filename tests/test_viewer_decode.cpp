#include <particle_codec/codec.h>
#include <particle_codec/frame_parser.h>
#include <iostream>
#include <vector>
#include <queue>
#include <cmath>
#include <cstring>
#include <algorithm>

#define NOMINMAX
#include <windows.h>

using namespace particle_codec;

struct Pixel {
    unsigned char r, g, b, a;
};

static bool save_bmp(const char *path, int width, int height, const std::vector<Pixel> &pixels) {
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bfh.bfType = 0x4D42;
    bfh.bfSize = 54 + width * height * 3;
    bfh.bfOffBits = 54;
    bih.biSize = 40;
    bih.biWidth = width;
    bih.biHeight = -height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    std::vector<unsigned char> bmp(54 + width * height * 3);
    memcpy(bmp.data(), &bfh, 14);
    memcpy(bmp.data() + 14, &bih, 40);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int si = y * width + x;
            int di = 54 + (y * width + x) * 3;
            bmp[di] = pixels[si].b;
            bmp[di + 1] = pixels[si].g;
            bmp[di + 2] = pixels[si].r;
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);
    return true;
}

struct Contour {
    double cx, cy;
    int area;
};

int main() {
    const int gridCols = 60, gridRows = 60;
    const int scale = 8;
    const int W = gridCols * scale;
    const int H = gridRows * scale;

    std::string message = "Hello viewer test!";

    std::cout << "=== Viewer-style encode→render→decode test ===" << std::endl;
    std::cout << "Message: " << message << std::endl;

    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto frames = codec.encode(std::vector<uint8_t>(message.begin(), message.end()));

    if (frames.empty()) {
        std::cerr << "FAIL: no frames" << std::endl;
        return 1;
    }

    auto &frame = frames[0];
    std::cout << "Encoded: " << frame.particleCount << " particles" << std::endl;

    // Render like viewer export: px * scale, py * scale, with updated smaller particles
    std::vector<Pixel> pixels(W * H);
    for (auto &p: pixels) {
        p.r = 10;
        p.g = 10;
        p.b = 26;
        p.a = 255;
    }

    int pr = std::max((int) (0.35 * scale), 2);
    int gr = pr * 2;

    for (int i = 0; i < frame.particleCount; i++) {
        double px = frame.particles[i * 2];
        double py = frame.particles[i * 2 + 1];
        double sx = px * scale;
        double sy = py * scale;

        for (int dy = -gr; dy <= gr; dy++) {
            for (int dx = -gr; dx <= gr; dx++) {
                double dist = std::sqrt(dx * dx + dy * dy);
                if (dist > gr) continue;
                int x = (int) (sx + dx), y = (int) (sy + dy);
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                pixels[y * W + x] = {0, 100, 130, 255};
            }
        }

        for (int dy = -pr; dy <= pr; dy++) {
            for (int dx = -pr; dx <= pr; dx++) {
                double dist = std::sqrt(dx * dx + dy * dy);
                if (dist > pr) continue;
                int x = (int) (sx + dx), y = (int) (sy + dy);
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                pixels[y * W + x] = {0, 188, 212, 255};
            }
        }
    }

    save_bmp("viewer_test.bmp", W, H, pixels);
    std::cout << "Saved: viewer_test.bmp (" << W << "x" << H << ")" << std::endl;

    // Detect using color-based flood-fill (core pixels only)
    std::vector<bool> visited(W * H, false);
    std::vector<Contour> contours;
    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            if (visited[idx]) continue;
            auto &p = pixels[idx];
            if (!(p.r < 80 && p.g > 100 && p.b > 100 && p.b > p.r + 20 && p.g > p.r + 20)) continue;

            double sumX = 0, sumY = 0;
            int count = 0;
            std::queue<std::pair<int, int> > q;
            q.push({x, y});
            visited[idx] = true;

            while (!q.empty()) {
                auto [cx, cy] = q.front();
                q.pop();
                sumX += cx;
                sumY += cy;
                count++;
                for (int d = 0; d < 8; d++) {
                    int nx = cx + dx8[d], ny = cy + dy8[d];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int nidx = ny * W + nx;
                    if (visited[nidx]) continue;
                    auto &np = pixels[nidx];
                    if (!(np.r < 80 && np.g > 100 && np.b > 100 && np.b > np.r + 20 && np.g > np.r + 20)) continue;
                    visited[nidx] = true;
                    q.push({nx, ny});
                }
            }
            if (count >= 3) {
                contours.push_back({sumX / count, sumY / count, count});
            }
        }
    }

    std::cout << "Color contours detected: " << contours.size() << std::endl;

    // Grid snap
    std::vector<std::pair<double, double> > gridPoints;
    std::vector<bool> cellUsed(gridCols * gridRows, false);

    for (auto &c: contours) {
        double gx = c.cx / scale;
        double gy = c.cy / scale;
        int col = std::clamp(static_cast<int>(std::floor(gx)), 0, gridCols - 1);
        int row = std::clamp(static_cast<int>(std::floor(gy)), 0, gridRows - 1);
        int idx = row * gridCols + col;
        if (!cellUsed[idx]) {
            cellUsed[idx] = true;
            gridPoints.emplace_back(col + 0.5, row + 0.5);
        }
    }

    std::cout << "Unique cells: " << gridPoints.size() << " (expected " << frame.particleCount << ")" << std::endl;

    auto result = codec.decodeCentroids(gridPoints);
    if (result.has_value() && !result->empty()) {
        auto header = FrameHeader::tryParse(*result);
        if (header && header->payloadLength > 0) {
            std::string decoded(result->begin() + FrameHeader::totalSize,
                                result->begin() + FrameHeader::totalSize + header->payloadLength);
            std::cout << "Decoded: \"" << decoded << "\"" << std::endl;
            if (decoded == message) {
                std::cout << "PASS!" << std::endl;
                return 0;
            } else {
                std::cerr << "FAIL: mismatch" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "FAIL: bad header" << std::endl;
            return 1;
        }
    } else {
        std::cerr << "FAIL: decode returned nothing" << std::endl;
        return 1;
    }
}
