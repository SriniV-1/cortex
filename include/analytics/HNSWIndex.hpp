#pragma once
// HNSWIndex — Hierarchical Navigable Small World graph for approximate
// nearest-neighbor search over 8-float GameStateVec vectors.
//
// Reference: Malkov & Yashunin, "Efficient and Robust Approximate Nearest
// Neighbor using Hierarchical Navigable Small World Graphs", 2018.
//
// Parameters:
//   M              — max connections per node per layer (default 16)
//   efConstruction — beam width during insertion (default 200)
//
// Usage:
//   HNSWIndex idx(M, efConstruction);
//   idx.build(vectors);                     // batch insert
//   auto results = idx.search(query, k, ef); // returns (dist_sq, index) pairs

#include "analytics/GameStateIndex.hpp"   // GameStateVec, l2_dist_sq

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

namespace cortex::analytics {

class HNSWIndex {
public:
    explicit HNSWIndex(size_t M = 16, size_t efConstruction = 200);
    ~HNSWIndex() = default;

    HNSWIndex(const HNSWIndex&)            = delete;
    HNSWIndex& operator=(const HNSWIndex&) = delete;
    HNSWIndex(HNSWIndex&&)                 = delete;
    HNSWIndex& operator=(HNSWIndex&&)      = delete;

    // Insert a single vector with the given external id (index into the
    // caller's metadata arrays).
    void insert(size_t id, const GameStateVec& vec);

    // Search for the k nearest neighbors of query.
    // ef controls the beam width (higher = more accurate, slower).
    // Returns pairs of (squared L2 distance, id) sorted by distance ascending.
    std::vector<std::pair<float, size_t>> search(const GameStateVec& query,
                                                  size_t k,
                                                  size_t ef = 0) const;

    // Batch-build the index from a contiguous vector array.
    // ids are 0, 1, ..., vectors.size()-1.
    void build(const std::vector<GameStateVec>& vectors);

    size_t size()      const noexcept { return num_elements_; }
    size_t max_level() const noexcept { return max_level_; }

private:
    // ── Graph node ───────────────────────────────────────────────────────
    struct Node {
        GameStateVec vec{};
        size_t       external_id = 0;
        size_t       level       = 0;          // highest layer this node lives in
        // neighbors_[l] = neighbor list at layer l
        std::vector<std::vector<size_t>> neighbors;
    };

    // ── Parameters ───────────────────────────────────────────────────────
    size_t M_;                // max connections per layer
    size_t M_max0_;           // max connections at layer 0 (= 2*M)
    size_t ef_construction_;
    double m_L_;              // 1 / ln(M) — controls level distribution

    // ── Graph storage ────────────────────────────────────────────────────
    std::vector<Node>   nodes_;        // indexed by internal node id
    size_t              entry_point_ = 0;
    size_t              max_level_   = 0;
    size_t              num_elements_= 0;

    mutable std::mutex  build_mutex_;
    std::mt19937        rng_;

    // ── Helpers ──────────────────────────────────────────────────────────
    size_t random_level();

    // Greedy search from entry_point down to target_layer, returning the
    // closest node at that layer.
    size_t greedy_search(const GameStateVec& query, size_t entry,
                         size_t top_layer, size_t target_layer) const;

    // Beam search at a single layer. Returns up to ef closest candidates
    // as (dist_sq, internal_node_id) sorted ascending.
    std::vector<std::pair<float, size_t>>
    search_layer(const GameStateVec& query, size_t entry,
                 size_t ef, size_t layer) const;

    // Select up to M best neighbors from candidates for the given node,
    // using the simple heuristic (keep M closest).
    static std::vector<size_t>
    select_neighbors(const std::vector<std::pair<float, size_t>>& candidates,
                     size_t M);

    // Shrink a neighbor list to at most max_M entries, keeping closest.
    void shrink_neighbors(size_t node_id, size_t layer, size_t max_M);
};

} // namespace cortex::analytics
