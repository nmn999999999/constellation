#include <particle_codec/codec.h>
#include <iostream>
#include <vector>
#include <queue>
#include <cmath>
#include <cstring>

#define NOMINMAX
#include <windows.h>

using namespace particle_codec;

struct Pixel {
    unsigned char r, g, b, a;
};

static bool save_bmp(const char *path, int width, int height, const std::vector<unsigned char> &rgba) {
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
            int si = (y * width + x) * 4;
            int di = 54 + (y * width + x) * 3;
            bmp[di] = rgba[si + 2];
            bmp[di + 1] = rgba[si + 1];
            bmp[di + 2] = rgba[si];
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);
    return true;
}

int main() {
    const int W = 600, H = 600;
    const int gridCols = 60, gridRows = 60;

    std::string message = "Hello from decode_image test!";

    std::cout << "=== End-to-end decode test ===" << std::endl;
    std::cout << "Message: " << message << std::endl;

    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto frames = codec.encode(std::vector<uint8_t>(message.begin(), message.end()));
    std::cout << "Encoded into " << frames.size() << " frame(s)" << std::endl;

    if (frames.empty()) {
        std::cerr << "FAIL: no frames produced" << std::endl;
        return 1;
    }

    auto &frame = frames[0];
    std::cout << "Frame 0: " << frame.particleCount << " particles" << std::endl;

    std::vector<unsigned char> rgba(W * H * 4, 0);
    for (int i = 0; i < W * H; i++) {
        rgba[i * 4] = 13;
        rgba[i * 4 + 1] = 17;
        rgba[i * 4 + 2] = 23;
        rgba[i * 4 + 3] = 255;
    }

    for (int i = 0; i < frame.particleCount; i++) {
        float fx = frame.particles[i * 2];
        float fy = frame.particles[i * 2 + 1];
        int px = (int) ((fx / gridCols) * W);
        int py = (int) ((fy / gridRows) * H);
        px = std::max(0, std::min(W - 1, px));
        py = std::max(0, std::min(H - 1, py));

        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int sx = px + dx, sy = py + dy;
                if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
                double dist = std::sqrt(dx * dx + dy * dy);
                double intensity = std::max(0.0, 1.0 - dist / 3.0);
                int idx = (sy * W + sx) * 4;
                rgba[idx] = (unsigned char) (0 * intensity);
                rgba[idx + 1] = (unsigned char) (188 * intensity);
                rgba[idx + 2] = (unsigned char) (212 * intensity);
                rgba[idx + 3] = 255;
            }
        }
    }

    const char *testPath = "test_particle_field.bmp";
    if (!save_bmp(testPath, W, H, rgba)) {
        std::cerr << "FAIL: could not save test image" << std::endl;
        return 1;
    }
    std::cout << "Saved test image: " << testPath << std::endl;

    std::vector<Pixel> pixels(W * H);
    for (int i = 0; i < W * H; i++) {
        pixels[i].r = rgba[i * 4];
        pixels[i].g = rgba[i * 4 + 1];
        pixels[i].b = rgba[i * 4 + 2];
        pixels[i].a = rgba[i * 4 + 3];
    }

    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    std::vector<bool> visited(W * H, false);
    std::vector<std::pair<double, double> > pixelCentroids;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            if (visited[idx]) continue;
            if (!(pixels[idx].r < 80 && pixels[idx].g > 100 && pixels[idx].b > 100 &&
                  pixels[idx].b > pixels[idx].r + 20 && pixels[idx].g > pixels[idx].r + 20))
                continue;

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
                    if (!(pixels[nidx].r < 80 && pixels[nidx].g > 100 && pixels[nidx].b > 100 &&
                          pixels[nidx].b > pixels[nidx].r + 20 && pixels[nidx].g > pixels[nidx].r + 20))
                        continue;
                    visited[nidx] = true;
                    q.push({nx, ny});
                }
            }
            if (count >= 3) pixelCentroids.push_back({sumX / count, sumY / count});
        }
    }

    std::cout << "Detected " << pixelCentroids.size() << " particles" << std::endl;

    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (auto [x, y]: pixelCentroids) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    double rangeX = std::max(maxX - minX, 1.0);
    double rangeY = std::max(maxY - minY, 1.0);
    double padX = rangeX * 0.05, padY = rangeY * 0.05;

    std::cout << "Pixel bounds: X[" << minX << "," << maxX << "] Y[" << minY << "," << maxY << "]" << std::endl;
    std::cout << "Range: " << rangeX << "x" << rangeY << " pad: " << padX << "x" << padY << std::endl;

    std::vector<std::pair<double, double> > gridPoints;
    for (auto [x, y]: pixelCentroids) {
        double gx = ((x - minX) / rangeX) * (gridCols - 1);
        double gy = ((y - minY) / rangeY) * (gridRows - 1);
        gridPoints.push_back({gx + 0.5, gy + 0.5});
    }

    std::cout << "Grid sample: ";
    for (int i = 0; i < std::min(5, (int) gridPoints.size()); i++) {
        std::cout << "(" << gridPoints[i].first << "," << gridPoints[i].second << ") ";
    }
    std::cout << std::endl;

    ParticleCodec codec2("particle_codec", gridCols, gridRows);
    auto frameBytes = codec2.decodeCentroids(gridPoints);

    if (frameBytes.has_value() && !frameBytes->empty()) {
        auto header = FrameHeader::tryParse(*frameBytes);
        if (header && header->payloadLength > 0) {
            std::string decoded(
                frameBytes->begin() + FrameHeader::totalSize,
                frameBytes->begin() + FrameHeader::totalSize + header->payloadLength);
            std::cout << "Decoded: \"" << decoded << "\"" << std::endl;
            if (decoded == message) {
                std::cout << "PASS: roundtrip successful!" << std::endl;
            } else {
                std::cerr << "FAIL: decoded != original" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "FAIL: could not parse frame header" << std::endl;
            return 1;
        }
    } else {
        std::cerr << "FAIL: decode returned nothing" << std::endl;
        return 1;
    }

    std::cout << "PASS: roundtrip successful!" << std::endl;
    return 0;
}
