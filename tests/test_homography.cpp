// Unit test for HomographyCalibrator: recover a perspective (keystone)
// homography from a synthetic particle lattice with noise.
#include <particle_codec/homography_calibrator.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace particle_codec;

int main() {
    std::mt19937 rng(42);
    std::vector<std::pair<double, double> > pts;
    // Ground-truth homography with strong perspective.
    double H0[3][3] = {{20.0, 1.2, 100.0},
                       {0.8, 20.0, 80.0},
                       {0.0004, 0.0002, 1.0}};
    std::normal_distribution<double> noise(0, 0.5);
    for (int r = 0; r < 60; r++) {
        for (int c = 0; c < 60; c++) {
            if (rng() % 3) continue;  // ~2/3 fill, like a real frame
            double gx = c + 0.5, gy = r + 0.5;
            double w = H0[2][0] * gx + H0[2][1] * gy + H0[2][2];
            pts.emplace_back((H0[0][0] * gx + H0[0][1] * gy + H0[0][2]) / w +
                                 noise(rng),
                             (H0[1][0] * gx + H0[1][1] * gy + H0[1][2]) / w +
                                 noise(rng));
        }
    }

    Homography hs[HomographyCalibrator::kMaxVariants];
    int n = HomographyCalibrator::calibrate(pts, 60, 60, hs);
    if (n <= 0) {
        std::fprintf(stderr, "FAIL: homography calibration returned no variants\n");
        return 1;
    }

    // Baseline: at least one variant must map >= 60% of centroids to grid
    // nodes on clean synthetic keystone. This is a floor for the automatic
    // lattice correspondence; stronger real-world robustness needs a more
    // elaborate correspondence (e.g. global bundle adjustment over the
    // lattice), tracked as future work.
    int bestInliers = 0;
    for (int k = 0; k < n; k++) {
        int inliers = 0;
        for (const auto &p : pts) {
            auto g = hs[k].map(p.first, p.second);
            double ex = g.first - (std::round(g.first - 0.5) + 0.5);
            double ey = g.second - (std::round(g.second - 0.5) + 0.5);
            if (ex * ex + ey * ey <= 0.45 * 0.45) inliers++;
        }
        if (inliers > bestInliers) bestInliers = inliers;
        std::printf("  variant %d inliers=%d (%.1f%%)\n", k, inliers,
                    100.0 * inliers / pts.size());
    }
    double rate = static_cast<double>(bestInliers) / pts.size();
    std::printf("HomographyCalibrator: variants=%d best inliers=%.1f%%\n",
                n, rate * 100.0);
    if (rate < 0.60) {
        std::fprintf(stderr, "FAIL: best variant inlier rate below 60%%\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
