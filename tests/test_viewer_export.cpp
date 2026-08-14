#include <particle_codec/codec.h>
#include <particle_codec/pseudo_random.h>
#include "../demo/nebula_render.h"
#include <iostream>
#include <vector>
#include <cmath>

using namespace particle_codec;

int main() {
    const int gridCols = 60, gridRows = 60;
    const int exportW = gridCols * 8;
    const int exportH = gridRows * 8;

    std::string message = "Hello from viewer export!";

    std::cout << "=== Viewer Export Decode Test ===" << std::endl;
    std::cout << "Message: " << message << std::endl;
    std::cout << "Export size: " << exportW << "x" << exportH << ", scale=8" << std::endl;

    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto frames = codec.encode(std::vector<uint8_t>(message.begin(), message.end()));
    if (frames.empty()) {
        std::cerr << "FAIL: no frames" << std::endl;
        return 1;
    }

    auto &frame = frames[0];
    std::cout << "Particles: " << frame.particleCount << std::endl;

    // Shared nebula renderer (same look as the video generator).
    auto noiseSeed = PseudoRandom::deriveSeed("particle_codec_drift", "video_gen");
    PerlinNoise noise(noiseSeed);
    std::vector<nebula::Star> stars;
    stars.reserve(static_cast<size_t>(frame.particleCount));
    for (int i = 0; i < frame.particleCount; i++) {
        double x = frame.particles[i * 2], y = frame.particles[i * 2 + 1];
        int col = static_cast<int>(std::floor(x));
        int row = static_cast<int>(std::floor(y));
        double mag = 0.5 + 0.5 * noise.fbm(col * 0.05, row * 0.05, 2);
        double size = 0.4 + mag * mag * 1.2;
        double bright = 0.6 + mag * 0.4;
        stars.push_back({x, y, size, bright, 1.0});
    }

    const char *testPath = "viewer_export_test.bmp";
    nebula::render(stars, gridCols, gridRows, /*margin=*/0, /*markers=*/false, 0.0,
                   noise, testPath);
    std::cout << "Saved: " << testPath << " (" << exportW << "x" << exportH << ")" << std::endl;

    std::cout << std::endl;
    std::cout << "Run: decode_image.exe " << testPath << " " << gridCols << " " << gridRows <<
            std::endl;
    return 0;
}
