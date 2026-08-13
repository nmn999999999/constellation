#pragma once

#include <cmath>
#include <utility>
#include <vector>

namespace particle_codec {

    // A 2D projective transform (homography) mapping image pixels onto the
    // canonical unit grid (col + 0.5, row + 0.5). Unlike the affine
    // GridCalibrator, this handles perspective (keystone) distortion from
    // photographing a flat field at an angle.
    struct Homography {
        double h[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        bool valid = false;

        std::pair<double, double> map(double x, double y) const {
            double w = h[2][0] * x + h[2][1] * y + h[2][2];
            if (std::abs(w) < 1e-12) return {0, 0};
            return {(h[0][0] * x + h[0][1] * y + h[0][2]) / w,
                    (h[1][0] * x + h[1][1] * y + h[1][2]) / w};
        }
    };

    // Estimates a homography from detected particle centroids of a (possibly
    // perspective-distorted) grid. Lattice indices are recovered by local
    // nearest-neighbour propagation, which tolerates the slowly varying
    // spacing/angle of a keystoned field, then the homography is fit by DLT +
    // Gauss-Newton with outlier rejection. All 8 square symmetries of the
    // canonical grid are returned; callers try them until one decodes.
    class HomographyCalibrator {
    public:
        static constexpr int kMaxVariants = 8;

        // Returns the number of valid orientation variants (0 on failure).
        static int calibrate(
            const std::vector<std::pair<double, double> > &centroids,
            int gridCols, int gridRows, Homography out[kMaxVariants]);
    };

} // namespace particle_codec
