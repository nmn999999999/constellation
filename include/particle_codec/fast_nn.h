#pragma once

#include <vector>
#include <utility>
#include <cmath>

namespace particle_codec {

/**
 * Fast Nearest Neighbor Search using KD-Tree.
 *
 * This class provides O(n log n) nearest neighbor search for 2D points,
 * significantly faster than O(n²) brute force for large datasets.
 *
 * Note: This is a simple implementation without external dependencies.
 * For production use, consider using FLANN (https://github.com/mariusmuja/flann)
 */
class FastNearestNeighbor {
public:
    explicit FastNearestNeighbor(const std::vector<std::pair<double, double> > &points);
    ~FastNearestNeighbor();

    /**
     * Find the nearest neighbor distance for each point.
     *
     * @param points Query points
     * @return Vector of nearest distances (sqrt of squared distance)
     */
    std::vector<double> nearestDistances(const std::vector<std::pair<double, double> > &points);

    /**
     * Find the k nearest neighbors for each point.
     *
     * @param points Query points
     * @param k Number of neighbors to find
     * @return Vector of vectors with k nearest distances
     */
    std::vector<std::vector<double> > kNearestDistances(
        const std::vector<std::pair<double, double> > &points, int k);

    /**
     * Find the k nearest neighbors with indices.
     *
     * @param points Query points
     * @param k Number of neighbors to find
     * @return Vector of vectors with (distance, index) pairs
     */
    std::vector<std::vector<std::pair<double, int> > > kNearestNeighbors(
        const std::vector<std::pair<double, double> > &points, int k);

private:
    struct IndexedPoint {
        std::pair<double, double> pt;
        int index;
    };

    struct Node {
        std::pair<double, double> point;
        int index;
        int axis;  // split axis: 0 = x, 1 = y
        Node *left = nullptr;
        Node *right = nullptr;
    };

    Node *root_ = nullptr;
    std::vector<std::pair<double, double> > points_;
    int n_ = 0;

    // Build KD-Tree
    Node* buildTree(std::vector<IndexedPoint> &pts, int depth);
    // Find nearest neighbor (skips the point with index == skipIdx, -1 = none)
    void findNearest(Node *node, const std::pair<double, double> &query,
                     double &bestDist, Node **bestNode, int &bestIdx, int skipIdx);
    // Find k nearest neighbors
    void findKNearest(Node *node, const std::pair<double, double> &query,
                      int k, std::vector<std::pair<double, int> > &candidates);
    // Clear tree
    void deleteTree(Node *node);
};

} // namespace particle_codec
