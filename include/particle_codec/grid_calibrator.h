#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace particle_codec {

    // Estimates the affine transform (rotation + scale + translation, plus a
    // small shear) that maps detected particle centroids in image pixel
    // coordinates onto the canonical unit grid used by the codec.
    //
    // The field is assumed to be a regular grid of particles (the standard
    // 60x60 export), possibly photographed at an angle, zoomed or shifted.
    // The fit is robust to missing particles (only ~50% of cells are filled)
    // and to moderate compression/noise, because it first recovers the grid
    // spacing and orientation from pairwise centroid distances, then finds a
    // lattice anchor by coverage voting and refines with least squares.
    class GridCalibrator {
    public:
        struct Affine {
            double a = 1, b = 0, c = 0; // x' = a*x + b*y + c
            double d = 0, e = 1, f = 0; // y' = d*x + e*y + f
            bool valid = false;

            std::pair<double, double> map(double x, double y) const {
                return {a * x + b * y + c, d * x + e * y + f};
            }

            // Rotate the output space by 90 degrees. The orientation of the
            // recovered lattice is ambiguous up to 90-degree multiples, so
            // callers should try all four variants and keep the one that
            // decodes.
            Affine rotated() const {
                return {-d, -e, -f, a, b, c, valid};
            }
        };

        // Least-squares affine fit: minimizes sum |T(p) - g|^2 over point
        // pairs (p in image space, g on the canonical grid). Returns false
        // when fewer than 3 pairs are given or the fit is degenerate.
        static bool fitAffine(const std::vector<std::pair<double, double> > &pts,
                              const std::vector<std::pair<double, double> > &grd,
                              Affine &out);

        // Fits the affine map so that mapped centroids land on grid node
        // centers (col + 0.5, row + 0.5). Returns an invalid Affine when the
        // field is too small, too sparse, or has no dominant spacing.
        static Affine calibrate(const std::vector<std::pair<double, double> > &centroids,
                                int gridCols, int gridRows,
                                double toleranceGrid = 0.35);

        // calibrate() then map every centroid to canonical grid coordinates.
        static std::vector<std::pair<double, double> > calibrateAndMap(
            const std::vector<std::pair<double, double> > &centroids,
            int gridCols, int gridRows, Affine *outTransform = nullptr);
    };

} // namespace particle_codec
