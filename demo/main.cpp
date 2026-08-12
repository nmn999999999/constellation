#include "particle_codec/codec.h"
#include <cmath>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

using namespace particle_codec;

void printParticles(const std::vector<EncodedFrame> &frames, int gridCols, int gridRows) {
    for (size_t f = 0; f < frames.size(); f++) {
        const auto &frame = frames[f];
        std::vector<std::string> grid(gridRows, std::string(gridCols, char(46)));

        for (int i = 0; i < frame.particleCount; i++) {
            float px = frame.particles[i * 2];
            float py = frame.particles[i * 2 + 1];
            int col = static_cast<int>(std::round(px));
            int row = static_cast<int>(std::round(py));
            if (col >= 0 && col < gridCols && row >= 0 && row < gridRows)
                grid[row][col] = char(35);
        }

        std::cout << "\n--- Frame " << f << " (" << frame.particleCount << " particles) ---\n";
        for (int r = 0; r < std::min(gridRows, 16); r++) {
            for (int c = 0; c < std::min(gridCols, 60); c++)
                std::cout << grid[r][c];
            std::cout << "\n";
        }
        if (gridRows > 16)
            std::cout << "... (" << gridRows << " rows total)\n";
    }
}

int main(int argc, char *argv[]) {
    std::string mode = "test";
    std::string input = "Hello Particle Codec!";
    bool useEcc = false;
    int gridSize = 20; // small grid for console visibility

    if (argc > 1) mode = argv[1];
    if (argc > 2) input = argv[2];
    if (argc > 3) useEcc = (std::string(argv[3]) == "ecc");

    // Override grid size for test modes
    if (mode == "test" || mode == "encode") {
        gridSize = 60;
    }

    ParticleCodec codec("particle_codec", gridSize, gridSize);

    if (mode == "test") {
        std::cout << "=== Particle Codec C++ Roundtrip Test ===\n";
        std::cout << "Input: \"" << input << "\" (" << input.size() << " bytes)\n";
        std::cout << "Grid: " << gridSize << "x" << gridSize << "\n";
        std::cout << "Max payload/frame: " << codec.maxPayloadBytes() << " bytes\n\n";

        auto data = std::vector<uint8_t>(input.begin(), input.end());
        auto frames = useEcc ? codec.encodeWithEcc(data) : codec.encode(data);

        std::cout << "Encoded " << frames.size() << " frame(s)\n";

        // Decode all frames
        FrameAssembler assembler;
        bool allOk = true;

        for (size_t i = 0; i < frames.size(); i++) {
            const auto &frame = frames[i];
            std::vector<std::pair<double, double> > centroids;
            for (int j = 0; j < frame.particleCount; j++)
                centroids.emplace_back(frame.particles[j * 2], frame.particles[j * 2 + 1]);

            auto decoded = useEcc
                               ? codec.decodeCentroidsWithEcc(centroids)
                               : codec.decodeCentroidsPayload(centroids);

            if (!decoded) {
                std::cout << "  Frame " << i << ": DECODE FAILED\n";
                allOk = false;
            } else {
                std::string text(decoded->begin(), decoded->end());
                std::cout << "  Frame " << i << ": " << frame.particleCount
                        << " particles, decoded: \"" << text << "\"\n";
                // For multi-frame, add to assembler
                auto header = FrameHeader::tryParse(
                    codec.decodeCentroids(centroids).value_or(std::vector<uint8_t>()));
                if (header) {
                    assembler.addFrame(header->seq, decoded.value());
                }
            }
        }

        // Try to assemble multi-frame
        auto assembled = assembler.extractAll();
        if (assembled) {
            std::string result(assembled->begin(), assembled->end());
            bool pass = (result == input);
            std::cout << "\nRoundtrip: " << (pass ? "PASS" : "FAIL") << "\n";
            std::cout << "Decoded: \"" << result << "\"\n";
            return pass ? 0 : 1;
        } else if (allOk && frames.size() == 1) {
            std::cout << "\nRoundtrip: PASS\n";
            return 0;
        }

        std::cout << "\nRoundtrip: FAIL\n";
        return 1;
    }

    if (mode == "encode") {
        auto data = std::vector<uint8_t>(input.begin(), input.end());
        auto frames = codec.encode(data);

        std::cout << "Encoded \"" << input << "\" into " << frames.size() << " frame(s), "
                << frames[0].particleCount << " particles\n";

        // Verify
        bool pass = ParticleCodec::verifyRoundtrip(data);
        std::cout << "Roundtrip verify: " << (pass ? "PASS" : "FAIL") << "\n";
        return pass ? 0 : 1;
    }

    if (mode == "visual") {
        // Generate a simple text visualization of the particle field
        auto data = std::vector<uint8_t>(input.begin(), input.end());
        auto frames = codec.encode(data);
        std::cout << "Text visualization of particle field for \"" << input << "\":\n";
        printParticles(frames, gridSize, gridSize);
        return 0;
    }

    std::cerr << "Usage: codec_demo [test|encode|visual] [input_text] [ecc]\n";
    return 1;
}
