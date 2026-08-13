#include "particle_codec/homography_calibrator.h"
#include "particle_codec/grid_calibrator.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <random>
#include <vector>

namespace particle_codec {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTolCell = 0.45;

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

void matMul(const double A[3][3], const double B[3][3], double C[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            C[i][j] = 0;
            for (int k = 0; k < 3; k++) C[i][j] += A[i][k] * B[k][j];
        }
}

// Solve an n x n linear system by Gaussian elimination with partial pivoting.
bool solveN(std::vector<std::vector<double> > A, std::vector<double> b,
            std::vector<double> &x) {
    int n = static_cast<int>(A.size());
    for (int col = 0; col < n; col++) {
        int piv = col;
        for (int r = col + 1; r < n; r++)
            if (std::abs(A[r][col]) > std::abs(A[piv][col])) piv = r;
        if (std::abs(A[piv][col]) < 1e-14) return false;
        if (piv != col) {
            std::swap(A[col], A[piv]);
            std::swap(b[col], b[piv]);
        }
        for (int r = col + 1; r < n; r++) {
            double f = A[r][col] / A[col][col];
            for (int c = col; c < n; c++) A[r][c] -= f * A[col][c];
            b[r] -= f * b[col];
        }
    }
    x.assign(n, 0.0);
    for (int r = n - 1; r >= 0; r--) {
        double s = b[r];
        for (int c = r + 1; c < n; c++) s -= A[r][c] * x[c];
        x[r] = s / A[r][r];
    }
    return true;
}

// Normalized DLT: null vector of the 2n x 9 design matrix via inverse
// iteration on A^T A + lambda I.
bool fitDlt(const std::vector<std::pair<double, double> > &pts,
            const std::vector<std::pair<double, double> > &grd,
            double H[3][3]) {
    int n = static_cast<int>(pts.size());
    if (n < 4) return false;

    auto normalize = [&](const std::vector<std::pair<double, double> > &p,
                         double T[3][3]) {
        double mx = 0, my = 0;
        for (const auto &q : p) {
            mx += q.first;
            my += q.second;
        }
        mx /= n;
        my /= n;
        double meanD = 0;
        for (const auto &q : p)
            meanD += std::sqrt((q.first - mx) * (q.first - mx) +
                               (q.second - my) * (q.second - my));
        meanD = std::max(meanD / n, 1e-9);
        double s = std::sqrt(2.0) / meanD;
        T[0][0] = s; T[0][1] = 0; T[0][2] = -s * mx;
        T[1][0] = 0; T[1][1] = s; T[1][2] = -s * my;
        T[2][0] = 0; T[2][1] = 0; T[2][2] = 1;
    };
    double T[3][3], U[3][3];
    normalize(pts, T);
    normalize(grd, U);

    std::vector<std::vector<double> > G(9, std::vector<double>(9, 0.0));
    for (int i = 0; i < n; i++) {
        double x = T[0][0] * pts[i].first + T[0][1] * pts[i].second + T[0][2];
        double y = T[1][0] * pts[i].first + T[1][1] * pts[i].second + T[1][2];
        double u = U[0][0] * grd[i].first + U[0][1] * grd[i].second + U[0][2];
        double v = U[1][0] * grd[i].first + U[1][1] * grd[i].second + U[1][2];
        double row1[9] = {x, y, 1, 0, 0, 0, -u * x, -u * y, -u};
        double row2[9] = {0, 0, 0, x, y, 1, -v * x, -v * y, -v};
        for (int a = 0; a < 9; a++)
            for (int b = 0; b < 9; b++)
                G[a][b] += row1[a] * row1[b] + row2[a] * row2[b];
    }
    for (int a = 0; a < 9; a++) G[a][a] += 1e-9;

    std::vector<double> x(9, 1.0 / 3.0), y;
    for (int it = 0; it < 20; it++) {
        if (!solveN(G, x, y)) return false;
        double norm = 0;
        for (double v : y) norm += v * v;
        norm = std::sqrt(norm);
        if (norm < 1e-12) return false;
        for (double &v : y) v /= norm;
        x = y;
    }
    double Hn[3][3] = {{x[0], x[1], x[2]},
                       {x[3], x[4], x[5]},
                       {x[6], x[7], x[8]}};
    double Uinv[3][3] = {{1 / U[0][0], 0, -U[0][2] / U[0][0]},
                         {0, 1 / U[1][1], -U[1][2] / U[1][1]},
                         {0, 0, 1}};
    double tmp[3][3];
    matMul(Hn, T, tmp);
    matMul(Uinv, tmp, H);
    return true;
}

void refineHomography(const std::vector<std::pair<double, double> > &pts,
                      const std::vector<std::pair<double, double> > &grd,
                      double H[3][3]) {
    std::vector<int> active(pts.size());
    for (size_t i = 0; i < pts.size(); i++) active[i] = static_cast<int>(i);

    for (int round = 0; round < 4; round++) {
        std::vector<double> JtJ(64, 0.0), Jtr(8, 0.0);
        std::vector<int> next;
        for (int idx : active) {
            double x = pts[idx].first, y = pts[idx].second;
            double u = grd[idx].first, v = grd[idx].second;
            double w = H[2][0] * x + H[2][1] * y + 1.0;
            if (std::abs(w) < 1e-9) continue;
            double px = (H[0][0] * x + H[0][1] * y + H[0][2]) / w;
            double py = (H[1][0] * x + H[1][1] * y + H[1][2]) / w;
            double ex = px - u, ey = py - v;
            if (ex * ex + ey * ey <= kTolCell * kTolCell) next.push_back(idx);
            double jx[8] = {x / w, y / w, 1 / w, 0, 0, 0,
                            -px * x / w, -px * y / w};
            double jy[8] = {0, 0, 0, x / w, y / w, 1 / w,
                            -py * x / w, -py * y / w};
            for (int a = 0; a < 8; a++) {
                Jtr[a] += jx[a] * ex + jy[a] * ey;
                for (int b = 0; b < 8; b++)
                    JtJ[a * 8 + b] += jx[a] * jx[b] + jy[a] * jy[b];
            }
        }
        for (int a = 0; a < 8; a++) JtJ[a * 8 + a] += 1e-6;
        std::vector<std::vector<double> > A(8, std::vector<double>(8));
        for (int a = 0; a < 8; a++)
            for (int b = 0; b < 8; b++) A[a][b] = JtJ[a * 8 + b];
        std::vector<double> delta;
        if (!solveN(A, Jtr, delta)) return;
        H[0][0] -= delta[0]; H[0][1] -= delta[1]; H[0][2] -= delta[2];
        H[1][0] -= delta[3]; H[1][1] -= delta[4]; H[1][2] -= delta[5];
        H[2][0] -= delta[6]; H[2][1] -= delta[7];
        if (round < 3 && next.size() >= 4) active = next;
    }
}

} // namespace

int HomographyCalibrator::calibrate(
    const std::vector<std::pair<double, double> > &centroids,
    int gridCols, int gridRows, Homography out[kMaxVariants]) {
    const int n = static_cast<int>(centroids.size());
    if (n < 16 || gridCols < 2 || gridRows < 2) return 0;

    // 1) Dominant spacing candidates from the NN-distance histogram.
    std::vector<double> nnDists(n);
    for (int i = 0; i < n; i++) {
        double best = 1e18;
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            best = std::min(best, dist2(centroids[i], centroids[j]));
        }
        nnDists[i] = std::sqrt(best);
    }
    std::map<int, int> distHist;
    for (double d : nnDists) distHist[static_cast<int>(std::lround(d))]++;
    std::vector<int> spacingPx;
    for (int d = 1; d <= 200 && static_cast<int>(spacingPx.size()) < 4; d++) {
        if (distHist[d] < 4) continue;
        if (distHist[d] >= distHist[d - 1] && distHist[d] >= distHist[d + 1])
            spacingPx.push_back(d);
    }

    auto estimateOrientation = [&](double s, double &theta0) {
        std::vector<int> angHist(60, 0);
        double lo = s * 0.85, hi = s * 1.15;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                double dx = centroids[j].first - centroids[i].first;
                double dy = centroids[j].second - centroids[i].second;
                double d = std::sqrt(dx * dx + dy * dy);
                if (d < lo || d > hi) continue;
                angHist[static_cast<int>(angleOf(dx, dy) / 3.0) % 60]++;
            }
        int bestBin = static_cast<int>(
            std::max_element(angHist.begin(), angHist.end()) - angHist.begin());
        theta0 = (bestBin + 0.5) * 3.0;
        if (std::sin((theta0 + 90.0) * kPi / 180.0) < 0) theta0 += 180.0;
        if (theta0 >= 180.0) theta0 -= 180.0;
    };

    double bestCoverage = 0;
    Homography best;
    for (int sp : spacingPx) {
        const double s = static_cast<double>(sp);
        double theta0 = 0;
        estimateOrientation(s, theta0);
        const double r0 = theta0 * kPi / 180.0;
        const double r1 = (theta0 + 90.0) * kPi / 180.0;
        const std::pair<double, double> u{std::cos(r0), std::sin(r0)};
        const std::pair<double, double> v{std::cos(r1), std::sin(r1)};

        // 2) Seed: centroid with the most neighbours within 1.45 cells.
        int seed = -1, seedCnt = 0;
        for (int i = 0; i < n; i++) {
            int cnt = 0;
            for (int j = 0; j < n; j++) {
                if (i == j) continue;
                if (dist2(centroids[i], centroids[j]) <= 1.45 * 1.45 * s * s)
                    cnt++;
            }
            if (cnt > seedCnt) {
                seedCnt = cnt;
                seed = i;
            }
        }
        if (seed < 0 || seedCnt < 4) continue;

        // 3) Lattice propagation from the seed with unit lattice steps.
        std::vector<int> ci(n, -1), cj(n, -1);
        std::vector<char> visited(n, 0);
        std::queue<int> q;
        ci[seed] = 0;
        cj[seed] = 0;
        visited[seed] = 1;
        q.push(seed);
        while (!q.empty()) {
            int p = q.front();
            q.pop();
            for (int j = 0; j < n; j++) {
                if (visited[j]) continue;
                double dx = centroids[j].first - centroids[p].first;
                double dy = centroids[j].second - centroids[p].second;
                double len = std::sqrt(dx * dx + dy * dy);
                if (len < 0.55 * s || len > 1.55 * s) continue;
                double a = (dx * u.first + dy * u.second) / s;
                double b = (dx * v.first + dy * v.second) / s;
                int ra = static_cast<int>(std::lround(a));
                int rb = static_cast<int>(std::lround(b));
                if (ra == 0 && rb == 0) continue;
                if (std::abs(ra) > 1 || std::abs(rb) > 1) continue;
                double ea = std::abs(a - ra), eb = std::abs(b - rb);
                if (ea > 0.42 || eb > 0.42) continue;
                ci[j] = ci[p] + ra;
                cj[j] = cj[p] + rb;
                visited[j] = 1;
                q.push(j);
            }
        }

        std::vector<std::pair<double, double> > pts, grd;
        int minCi = ci[seed], minCj = cj[seed];
        for (int i = 0; i < n; i++) {
            if (!visited[i]) continue;
            minCi = std::min(minCi, ci[i]);
            minCj = std::min(minCj, cj[i]);
            pts.push_back(centroids[i]);
            grd.emplace_back(ci[i] - minCi + 0.5, cj[i] - minCj + 0.5);
        }
        if (pts.size() < 12) continue;

        // 4) Direct DLT on the propagated correspondences.
        Homography h;
        if (!fitDlt(pts, grd, h.h)) continue;
        refineHomography(pts, grd, h.h);

        // 5) Rebuild correspondences by reprojection and refit (recovers
        //    points the propagation missed).
        for (int iter = 0; iter < 2; iter++) {
            std::vector<std::pair<double, double> > pts2, grd2;
            for (int i = 0; i < n; i++) {
                auto m = h.map(centroids[i].first, centroids[i].second);
                double ri = std::round(m.first - 0.5);
                double rj = std::round(m.second - 0.5);
                if (ri < -1 || rj < -1 || ri > gridCols + 1 ||
                    rj > gridRows + 1)
                    continue;
                double ex = m.first - (ri + 0.5);
                double ey = m.second - (rj + 0.5);
                if (ex * ex + ey * ey <= kTolCell * kTolCell) {
                    pts2.push_back(centroids[i]);
                    grd2.emplace_back(ri + 0.5, rj + 0.5);
                }
            }
            if (pts2.size() < 12) break;
            if (!fitDlt(pts2, grd2, h.h)) break;
            refineHomography(pts2, grd2, h.h);
        }

        double coverage = 0;
        for (int i = 0; i < n; i++) {
            auto m = h.map(centroids[i].first, centroids[i].second);
            double ex = m.first - (std::round(m.first - 0.5) + 0.5);
            double ey = m.second - (std::round(m.second - 0.5) + 0.5);
            if (ex * ex + ey * ey <= kTolCell * kTolCell) coverage++;
        }
        coverage /= n;
        if (coverage > bestCoverage) {
            bestCoverage = coverage;
            best = h;
            best.valid = true;
        }
        if (coverage > 0.85) break;
    }
    if (!best.valid || bestCoverage < 0.5) return 0;

    // 6) Orientation variants: compose with the 8 symmetries of the square
    //    grid [0,cols) x [0,rows).
    const int C = gridCols, R = gridRows;
    const double S[8][3][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},                          // id
        {{0, -1, R}, {1, 0, 0}, {0, 0, 1}},                         // R90
        {{-1, 0, C}, {0, -1, R}, {0, 0, 1}},                        // R180
        {{0, 1, 0}, {-1, 0, C}, {0, 0, 1}},                         // R270
        {{-1, 0, C}, {0, 1, 0}, {0, 0, 1}},                         // mirror X
        {{1, 0, 0}, {0, -1, R}, {0, 0, 1}},                         // mirror Y
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},                          // transpose
        {{0, -1, R}, {-1, 0, C}, {0, 0, 1}},                        // anti-tr.
    };
    int count = 0;
    for (int k = 0; k < kMaxVariants; k++) {
        Homography hv = best;
        double tmp[3][3];
        matMul(S[k], best.h, tmp);  // hv = S_k * best (applied to grid space)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) hv.h[i][j] = tmp[i][j];
        hv.valid = true;
        out[count++] = hv;
    }
    return count;
}

} // namespace particle_codec
