#include "particle_codec/fast_nn.h"
#include <algorithm>
#include <queue>
#include <limits>

namespace particle_codec {

FastNearestNeighbor::FastNearestNeighbor(const std::vector<std::pair<double, double> > &points)
    : points_(points), n_(static_cast<int>(points.size())) {

    if (n_ == 0) return;

    // Build KD-Tree, carrying original indices through the sort
    std::vector<IndexedPoint> indexed;
    indexed.reserve(n_);
    for (int i = 0; i < n_; i++) indexed.push_back({points_[i], i});
    root_ = buildTree(indexed, 0);
}

FastNearestNeighbor::~FastNearestNeighbor() {
    deleteTree(root_);
}

std::vector<double> FastNearestNeighbor::nearestDistances(
    const std::vector<std::pair<double, double> > &query) {

    std::vector<double> result;
    result.reserve(query.size());

    // Query set is expected to be the same set the tree was built from
    // (same order). Each query point therefore skips itself by index.
    bool sameSet = (query.size() == static_cast<size_t>(n_));
    if (sameSet) {
        for (int i = 0; i < static_cast<int>(query.size()); i++) {
            double bestDist = std::numeric_limits<double>::max();
            int bestIdx = -1;
            findNearest(root_, query[i], bestDist, nullptr, bestIdx, i);
            result.push_back(std::sqrt(bestDist));
        }
    } else {
        for (const auto &q : query) {
            double bestDist = std::numeric_limits<double>::max();
            int bestIdx = -1;
            findNearest(root_, q, bestDist, nullptr, bestIdx, -1);
            result.push_back(std::sqrt(bestDist));
        }
    }

    return result;
}

std::vector<std::vector<double> > FastNearestNeighbor::kNearestDistances(
    const std::vector<std::pair<double, double> > &query, int k) {

    std::vector<std::vector<double> > result;
    result.reserve(query.size());

    for (const auto &q : query) {
        std::vector<std::pair<double, int> > candidates;
        findKNearest(root_, q, k, candidates);
        std::vector<double> distances;
        for (const auto &c : candidates) {
            distances.push_back(std::sqrt(c.first));
        }
        result.push_back(distances);
    }

    return result;
}

std::vector<std::vector<std::pair<double, int> > > FastNearestNeighbor::kNearestNeighbors(
    const std::vector<std::pair<double, double> > &query, int k) {

    std::vector<std::vector<std::pair<double, int> > > result;
    result.reserve(query.size());

    for (const auto &q : query) {
        std::vector<std::pair<double, int> > candidates;
        findKNearest(root_, q, k, candidates);
        result.push_back(candidates);
    }

    return result;
}

FastNearestNeighbor::Node* FastNearestNeighbor::buildTree(
    std::vector<IndexedPoint> &pts, int depth) {

    if (pts.empty()) return nullptr;

    int axis = depth % 2;  // 0 = x, 1 = y

    // Sort by axis
    std::sort(pts.begin(), pts.end(), [axis](const IndexedPoint &a, const IndexedPoint &b) {
        return axis == 0 ? a.pt.first < b.pt.first : a.pt.second < b.pt.second;
    });

    int mid = static_cast<int>(pts.size()) / 2;
    Node *node = new Node();
    node->point = pts[mid].pt;
    node->index = pts[mid].index;
    node->axis = axis;

    // Build subtrees from copies (small n; simplicity over allocation cost)
    std::vector<IndexedPoint> left(pts.begin(), pts.begin() + mid);
    std::vector<IndexedPoint> right(pts.begin() + mid + 1, pts.end());
    node->left = buildTree(left, depth + 1);
    node->right = buildTree(right, depth + 1);

    return node;
}

void FastNearestNeighbor::findNearest(Node *node, const std::pair<double, double> &query,
                                       double &bestDist, Node **bestNode, int &bestIdx,
                                       int skipIdx) {
    if (!node) return;

    if (node->index != skipIdx) {
        double dx = query.first - node->point.first;
        double dy = query.second - node->point.second;
        double dist = dx * dx + dy * dy;

        // Update best
        if (dist < bestDist) {
            bestDist = dist;
            if (bestNode) *bestNode = node;
            bestIdx = node->index;
        }
    }

    // Distance along the split axis for pruning
    double axisDist = (node->axis == 0)
                          ? query.first - node->point.first
                          : query.second - node->point.second;

    // Recurse on closer side first
    if (axisDist < 0) {
        findNearest(node->left, query, bestDist, bestNode, bestIdx, skipIdx);
        // Check if we need to search the other side
        if (axisDist * axisDist >= bestDist) {
            findNearest(node->right, query, bestDist, bestNode, bestIdx, skipIdx);
        }
    } else {
        findNearest(node->right, query, bestDist, bestNode, bestIdx, skipIdx);
        if (axisDist * axisDist >= bestDist) {
            findNearest(node->left, query, bestDist, bestNode, bestIdx, skipIdx);
        }
    }
}

void FastNearestNeighbor::findKNearest(Node *node, const std::pair<double, double> &query,
                                        int k, std::vector<std::pair<double, int> > &candidates) {
    if (!node) return;

    double dx = query.first - node->point.first;
    double dy = query.second - node->point.second;
    double dist = dx * dx + dy * dy;

    // Add to candidates
    candidates.push_back({dist, node->index});

    int limit = std::min(k, static_cast<int>(candidates.size()));

    // Keep only k smallest (if we have at least k)
    if (static_cast<int>(candidates.size()) >= k) {
        std::partial_sort(candidates.begin(), candidates.begin() + k, candidates.end());
    } else {
        std::sort(candidates.begin(), candidates.end());
    }

    // Distance along the split axis for pruning
    double axisDist = (node->axis == 0)
                          ? query.first - node->point.first
                          : query.second - node->point.second;

    if (axisDist < 0) {
        findKNearest(node->left, query, k, candidates);
        if (static_cast<int>(candidates.size()) >= k &&
            axisDist * axisDist < candidates[k - 1].first) {
            findKNearest(node->right, query, k, candidates);
        }
    } else {
        findKNearest(node->right, query, k, candidates);
        if (static_cast<int>(candidates.size()) >= k &&
            axisDist * axisDist < candidates[k - 1].first) {
            findKNearest(node->left, query, k, candidates);
        }
    }
}

void FastNearestNeighbor::deleteTree(Node *node) {
    if (!node) return;
    deleteTree(node->left);
    deleteTree(node->right);
    delete node;
}

} // namespace particle_codec
