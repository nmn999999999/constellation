#include "particle_codec/grid_calibrator.h"
#include "particle_codec/fast_nn.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <cstdio>

namespace particle_codec {

    namespace {
        const double kPi = 3.14159265358979323846;

        double dist2(const std::pair<double, double> &a,
                     const std::pair<double, double> &b) {
            double dx = a.first - b.first;
            double dy = a.second - b.second;
            return dx * dx + dy * dy;
        }

        double angleOf(double dx, double dy) {
            double ang = std::atan2(dy, dx) * 180.0 / kPi;
            if (ang < 0) ang += 180.0;
            if (ang >= 180.0) ang -= 180.0;
            return ang;
        }

    } // namespace

    bool GridCalibrator::fitAffine(
        const std::vector<std::pair<double, double> > &pts,
        const std::vector<std::pair<double, double> > &grd,
        GridCalibrator::Affine &out) {
            const int n = static_cast<int>(pts.size());
            if (n < 3) return false;
            // Centered normal equations for x' = a*x + b*y + c.
            double sxx = 0, syy = 0, sxy = 0, sx = 0, sy = 0;
            double sxgx = 0, sygx = 0, sgx = 0;
            double sxgy = 0, sygy = 0, sgy = 0;
            for (int i = 0; i < n; i++) {
                double x = pts[i].first, y = pts[i].second;
                double gx = grd[i].first, gy = grd[i].second;
                sxx += x * x; syy += y * y; sxy += x * y;
                sx += x; sy += y;
                sxgx += x * gx; sygx += y * gx; sgx += gx;
                sxgy += x * gy; sygy += y * gy; sgy += gy;
            }
            const double mx = sx / n, my = sy / n;
            const double mgx = sgx / n, mgy = sgy / n;
            const double sxxc = sxx - n * mx * mx;
            const double syyc = syy - n * my * my;
            const double sxyc = sxy - n * mx * my;
            const double sxgxc = sxgx - n * mx * mgx;
            const double sygxc = sygx - n * my * mgx;
            const double sxgyc = sxgy - n * mx * mgy;
            const double sygyc = sygy - n * my * mgy;
            const double det = sxxc * syyc - sxyc * sxyc;
            if (std::abs(det) < 1e-9) return false;
            out.a = (sxgxc * syyc - sygxc * sxyc) / det;
            out.b = (sygxc * sxxc - sxgxc * sxyc) / det;
            out.c = mgx - out.a * mx - out.b * my;
            out.d = (sxgyc * syyc - sygyc * sxyc) / det;
            out.e = (sygyc * sxxc - sxgyc * sxyc) / det;
            out.f = mgy - out.d * mx - out.e * my;
            return true;
    }

    GridCalibrator::Affine GridCalibrator::calibrate(
        const std::vector<std::pair<double, double> > &centroids,
        int gridCols, int gridRows, double toleranceGrid) {
        Affine invalid;
        const int n = static_cast<int>(centroids.size());
        if (n < 16 || gridCols < 2 || gridRows < 2 || toleranceGrid <= 0) return invalid;

        // 1) Dominant spacing from nearest-neighbour distances.
        // Using KD-Tree for O(n log n) performance instead of O(n²)
        std::vector<double> nnDists;
        nnDists.reserve(n);
        FastNearestNeighbor knn(centroids);
        nnDists = knn.nearestDistances(centroids);

        std::map<int, int> distHist;
        for (double d: nnDists) distHist[static_cast<int>(std::lround(d))]++;
        // Candidate spacings: local maxima of the NN-distance histogram,
        // ordered by distance. The true spacing is the *smallest* spacing at
        // which adjacent cells both carry particles; in sparse fields the
        // taller peaks belong to multiples of the true spacing, so we try
        // several candidates and keep the best-fitting one.
        std::vector<int> peaks;
        for (int d = 1; d <= 160 && peaks.size() < 5; d++) {
            if (distHist[d] < 4) continue;
            if (distHist[d] >= distHist[d - 1] && distHist[d] >= distHist[d + 1])
                peaks.push_back(d);
        }

        Affine best;
        double bestCoverage = 0;

        for (int spacingPx: peaks) {
            const double s = static_cast<double>(spacingPx);

            // 2) Orientation histogram over near-neighbour pairs.
            // Window excludes diagonal pairs (1.41x spacing) so the histogram
            // peaks at the true grid axes, not the diagonals.
            const double lo = s * 0.85, hi = s * 1.15;
            std::vector<int> angHist(60, 0); // 3-degree bins over 180
            std::vector<std::pair<double, double> > pairVectors;
            for (int i = 0; i < n; i++) {
                for (int j = i + 1; j < n; j++) {
                    double dx = centroids[j].first - centroids[i].first;
                    double dy = centroids[j].second - centroids[i].second;
                    double d2 = dx * dx + dy * dy;
                    double lo2 = lo * lo, hi2 = hi * hi;
                    if (d2 < lo2 || d2 > hi2) continue;
                    double ang = angleOf(dx, dy);
                    angHist[static_cast<int>(ang / 3.0) % 60]++;
                    pairVectors.emplace_back(dx, dy);
                }
            }
            if (pairVectors.empty()) continue;

            int bestBin = static_cast<int>(
                std::max_element(angHist.begin(), angHist.end()) - angHist.begin());
            double theta0 = (bestBin + 0.5) * 3.0; // deg
            // Second orientation: peak near theta0 + 90.
            int bestBin2 = -1, bestCount2 = 0;
            for (int b = 0; b < 60; b++) {
                double ang = b * 3.0;
                double delta = std::abs(ang - (theta0 + 90.0));
                if (delta > 90.0) delta = 180.0 - delta;
                if (delta > 15.0) continue;
                if (angHist[b] > bestCount2) {
                    bestCount2 = angHist[b];
                    bestBin2 = b;
                }
            }
            double theta1 = (bestBin2 >= 0)
                                ? (bestBin2 + 0.5) * 3.0
                                : theta0 + 90.0;

            // 3) Joint (spacing x angle) refinement: the coarse 3-degree bins
            // and integer spacing accumulate up to ~1 cell of error at the
            // field edges, so sweep fine steps and keep the combination with
            // the best average coverage over a small anchor sample.
            std::vector<int> anchors;
            for (int i = 0; i < n && anchors.size() < 48; i++) anchors.push_back(i);
            {
                std::mt19937 rng(12345);
                std::uniform_int_distribution<int> pick(0, n - 1);
                while (anchors.size() < 96) anchors.push_back(pick(rng));
            }

            double bestS = s, bestTh = theta0, bestSweepCov = -1;
            // Sweep over a subsample: this stage only localizes (spacing,
            // angle); the final least-squares fit uses every centroid.
            std::vector<int> sampleIdx;
            for (int i = 0; i < n && static_cast<int>(sampleIdx.size()) < 400; i++)
                sampleIdx.push_back(i);
            {
                std::mt19937 srng(777);
                std::uniform_int_distribution<int> spick(0, n - 1);
                while (static_cast<int>(sampleIdx.size()) < 400)
                    sampleIdx.push_back(spick(srng));
            }
            const int anchorCount = std::min(8, static_cast<int>(anchors.size()));

            for (double ss = s * 0.80; ss <= s * 1.25; ss += 0.03) {
                for (double th = theta0 - 1.8; th <= theta0 + 1.8; th += 0.1) {
                    double rr0 = th * kPi / 180.0;
                    double rr1 = (th + 90.0) * kPi / 180.0;
                    std::pair<double, double> uf{std::cos(rr0) * ss, std::sin(rr0) * ss};
                    std::pair<double, double> vf{std::cos(rr1) * ss, std::sin(rr1) * ss};
                    double detUV = uf.first * vf.second - uf.second * vf.first;
                    if (std::abs(detUV) < 1e-9) continue;

                    long long coveredSum = 0;
                    for (int k = 0; k < anchorCount; k++) {
                        const auto &p = centroids[anchors[k]];
                        for (int si: sampleIdx) {
                            double dx = centroids[si].first - p.first;
                            double dy = centroids[si].second - p.second;
                            double fi = (dx * vf.second - dy * vf.first) / detUV;
                            double fj = (uf.first * dy - uf.second * dx) / detUV;
                            double ri = std::round(fi), rj = std::round(fj);
                            double gx = p.first + ri * uf.first + rj * vf.first;
                            double gy = p.second + ri * uf.second + rj * vf.second;
                            double ex = centroids[si].first - gx;
                            double ey = centroids[si].second - gy;
                            if (ex * ex + ey * ey <=
                                ss * ss * toleranceGrid * toleranceGrid)
                                coveredSum++;
                        }
                    }
                    double cov = static_cast<double>(coveredSum) /
                                 (static_cast<double>(anchorCount) * sampleIdx.size());
                    if (cov > bestSweepCov) {
                        bestSweepCov = cov;
                        bestS = ss;
                        bestTh = th;
                    }
                }
            }

            // Normalize orientation so v points downward (which also forces u
            // rightward): the angle histogram is symmetric under 180-degree
            // flips, and an upside-down basis would make the fitted map mirror
            // the grid, which no rotation variant can decode.
            double thNorm = bestTh;
            if (std::sin((thNorm + 90.0) * kPi / 180.0) < 0) thNorm += 180.0;
            const double r0 = thNorm * kPi / 180.0;
            const double r1 = (thNorm + 90.0) * kPi / 180.0;
            const std::pair<double, double> u{std::cos(r0) * bestS, std::sin(r0) * bestS};
            const std::pair<double, double> v{std::cos(r1) * bestS, std::sin(r1) * bestS};

            // 4) Anchor voting: try candidate centroids as lattice origin.
            const double tol = bestS * toleranceGrid;
            const double tol2 = tol * tol;
            double bestAnchorCoverage = 0;
            std::pair<double, double> bestAnchor;
            for (int ai: anchors) {
                const auto &p0 = centroids[ai];
                int covered = 0;
                for (int i = 0; i < n; i++) {
                    double dx = centroids[i].first - p0.first;
                    double dy = centroids[i].second - p0.second;
                    // Solve [u v]^{-1} * (d) for fractional lattice coords.
                    double detUV = u.first * v.second - u.second * v.first;
                    if (std::abs(detUV) < 1e-9) break;
                    double fi = (dx * v.second - dy * v.first) / detUV;
                    double fj = (u.first * dy - u.second * dx) / detUV;
                    double ri = std::round(fi), rj = std::round(fj);
                    double gx = p0.first + ri * u.first + rj * v.first;
                    double gy = p0.second + ri * u.second + rj * v.second;
                    double ex = centroids[i].first - gx;
                    double ey = centroids[i].second - gy;
                    if (ex * ex + ey * ey <= tol2) covered++;
                }
                double coverage = static_cast<double>(covered) / n;
                if (coverage > bestAnchorCoverage) {
                    bestAnchorCoverage = coverage;
                    bestAnchor = p0;
                }
            }
            if (bestAnchorCoverage < 0.55) continue;

            // 5) Build point pairs within tolerance, then least-squares fit.
            std::vector<std::pair<double, double> > pts, grd;
            const auto &p0 = bestAnchor;
            for (int i = 0; i < n; i++) {
                double dx = centroids[i].first - p0.first;
                double dy = centroids[i].second - p0.second;
                double detUV = u.first * v.second - u.second * v.first;
                double fi = (dx * v.second - dy * v.first) / detUV;
                double fj = (u.first * dy - u.second * dx) / detUV;
                double ri = std::round(fi), rj = std::round(fj);
                double gx = p0.first + ri * u.first + rj * v.first;
                double gy = p0.second + ri * u.second + rj * v.second;
                double ex = centroids[i].first - gx;
                double ey = centroids[i].second - gy;
                if (ex * ex + ey * ey <= tol2) {
                    pts.push_back(centroids[i]);
                    grd.emplace_back(ri + 0.5, rj + 0.5);
                }
            }
            if (pts.size() < 9) continue;

            Affine fitted;
            if (!fitAffine(pts, grd, fitted)) continue;

            // 6) Re-evaluate coverage with the refined transform; iterate once.
            double refinedCoverage = 0;
            for (int iter = 0; iter < 2; iter++) {
                std::vector<std::pair<double, double> > pts2, grd2;
                for (int i = 0; i < n; i++) {
                    double fx = centroids[i].first, fy = centroids[i].second;
                    auto m = fitted.map(fx, fy);
                    double ri = std::round(m.first - 0.5);
                    double rj = std::round(m.second - 0.5);
                    double gx = ri + 0.5, gy = rj + 0.5;
                    double ex = m.first - gx, ey = m.second - gy;
                    if (ex * ex + ey * ey <= toleranceGrid * toleranceGrid) {
                        pts2.push_back(centroids[i]);
                        grd2.emplace_back(gx, gy);
                    }
                }
                refinedCoverage = static_cast<double>(pts2.size()) / n;
                if (pts2.size() >= 9) {
                    if (!fitAffine(pts2, grd2, fitted)) break;
                }
            }

            if (refinedCoverage > bestCoverage && refinedCoverage >= 0.60) {
                bestCoverage = refinedCoverage;
                best = fitted;
                best.valid = true;
            }
            if (bestSweepCov > 0.92) break; // first candidate already fits well
        }

        return best;
    }

    std::vector<std::pair<double, double> > GridCalibrator::calibrateAndMap(
        const std::vector<std::pair<double, double> > &centroids,
        int gridCols, int gridRows, Affine *outTransform) {
        auto t = calibrate(centroids, gridCols, gridRows);
        std::vector<std::pair<double, double> > mapped;
        if (!t.valid) {
            if (outTransform) *outTransform = t;
            return mapped;
        }

        // The fitted map is anchored at an arbitrary particle, so mapped
        // coordinates can be negative or shifted by many cells. Shift by a
        // whole number of cells so the smallest mapped coordinate sits on grid
        // node 0 (col/row 0) and decoding sees coordinates in [0, gridSize).
        double minX = 1e18, minY = 1e18;
        for (const auto &p: centroids) {
            auto m = t.map(p.first, p.second);
            minX = std::min(minX, m.first);
            minY = std::min(minY, m.second);
        }
        t.c -= std::round(minX - 0.5);
        t.f -= std::round(minY - 0.5);
        if (outTransform) *outTransform = t;

        mapped.reserve(centroids.size());
        for (const auto &p: centroids) mapped.push_back(t.map(p.first, p.second));
        return mapped;
    }

} // namespace particle_codec
