/**
 * Demo: Fast Nearest Neighbor Search
 *
 * This example compares O(n²) brute force vs O(n log n) KD-Tree for
 * finding nearest neighbors in a particle field.
 */

#include "particle_codec/fast_nn.h"
#include "particle_codec/codec.h"
#include <iostream>
#include <vector>
#include <chrono>

using namespace particle_codec;
using namespace std;

// O(n²) brute force (original implementation)
std::vector<double> nearestDistancesBruteForce(
    const std::vector<std::pair<double, double> > &centroids) {

    int n = static_cast<int>(centroids.size());
    std::vector<double> result(n);

    for (int i = 0; i < n; i++) {
        double best = 1e18;
        for (int j = 0; KDTree j < n; j++) {
            if (i == j) continue;
            double dx = centroids[i].first - centroids[j].first;
            double dy = centroids[i].second - centroids[j].second;
            double d = dx * dx + dy * dy;
            if (d < best) best = d;
        }
        result[i] = std::sqrt(best);
    }

    return result;
}

int main() {
    // Generate test data (3600 particles for 60x60 grid)
    ParticleCodec codec("test", 60, 60);
    std::string testMessage = "Hello, Fast Nearest Neighbor!";
    auto data = std::vector<uint8_t>(testMessage.begin(), testMessage.end());
    auto frames = codec.encode(data);
    auto &frame = frames[0];

    std::vector<std::pair<double, double> > centroids;
    for (int i = 0; i < frame.particleCount; i++) {
        centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);
    }

    std::cout << "Test data: " << centroids.size() << " particles\n";

    // Benchmark brute force
    auto start = chrono::high_resolution_clock::now();
    auto bfResult = nearestDistancesBruteForce(centroids);
    auto bfEnd = chrono::high_resolution_clock::now();
    auto bfDuration = chrono::duration_cast<chrono::milliseconds>(bfEnd - start);

    std::cout << "Brute force: " << bfDuration.count() << " ms\n";

    // Benchmark KD-Tree
    start = chrono::high_resolution_clock::now();
    FastNearestNeighbor knn(centroids);
    auto kdResult = knn.nearestDistances(centroids);
    auto kdEnd = chrono::high_resolution_clock::now();
    auto kdDuration = chrono::duration_cast<chrono::milliseconds>(kdEnd - start);

    std::cout << "KD-Tree: " << kdDuration.count() << " ms\n";

    // Compare results
    double maxDiff = 0.0;
    for (size_t i = 0; i < bfResult.size(); i++) {
        double diff = std::abs(bfResult[i] - kdResult[i]);
        maxDiff = std::max(maxDiff, diff);
    }

    std::cout << "Max difference: " << maxDiff << "\n";

    // Performance improvement
    double improvement = (1.0 - static_cast<double>(kdDuration.count()) / bfDuration.count()) * 100.0;
    std::cout << "Speedup: " << improvement << "%\n";

    return 0;
}
