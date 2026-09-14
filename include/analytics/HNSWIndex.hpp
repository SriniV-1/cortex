#pragma once
// HNSWIndex — Hierarchical Navigable Small World graph for approximate
// nearest-neighbor search over 8-float GameStateVec vectors.
//
// Reference: Malkov & Yashunin, "Efficient and Robust Approximate Nearest
// Neighbor using Hierarchical Navigable Small World Graphs", 2018.
//
// Memory layout (sized for ~4.7M vectors on a 2 GB host):
//   - build() does NOT copy vectors — the graph reads the caller's array, which
//     must outlive the index (GameStateIndex owns it).
//   - Layer-0 links: one flat uint32 array, (M0 + 1) slots per node, slot 0 = count.
//   - Upper-layer links: one flat uint32 pool, (M + 1) slots per (node, layer).
//   - Visited set: thread-local generation-stamped array, no hashing per query.
//
// Parameters:
//   M              — max links per node on upper layers (layer 0 allows 2*M)
//   efConstruction — beam width during insertion
//   efSearch       — default beam width during queries
//
// Usage:
//   HNSWIndex idx(M, efConstruction);
//   idx.build(vectors);                      // batch build, ids = 0..N-1
//   auto results = idx.search(query, k, ef); // (dist_sq, id) pairs, ascending

#include "analytics/GameStateIndex.hpp"   // GameStateVec, l2_dist_sq

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace cortex::analytics {

class HNSWIndex {
public:
    explicit HNSWIndex(size_t M = 16, size_t efConstruction = 200, size_t efSearch = 64);

    HNSWIndex(const HNSWIndex&)            = delete;
    HNSWIndex& operator=(const HNSWIndex&) = delete;
    HNSWIndex(HNSWIndex&&)                 = default;
    HNSWIndex& operator=(HNSWIndex&&)      = default;

    // Insert a single vector (copied) with the given external id.
    // Use either insert() or build() on one index, not both.
    void insert(size_t id, const GameStateVec& vec);

    // Search for the k nearest neighbors of query. ef = beam width
    // (0 → efSearch). Returns (squared L2 distance, id) sorted ascending.
    std::vector<std::pair<float, size_t>> search(const GameStateVec& query,
                                                  size_t k,
                                                  size_t ef = 0) const;

    // Batch-build over `vectors` (ids 0..N-1) without copying them.
    void build(const std::vector<GameStateVec>& vectors);

    size_t size()         const noexcept { return levels_.size(); }
    size_t max_level()    const noexcept { return max_level_; }
    size_t memory_bytes() const noexcept;

    // Number of nodes reachable from the entry point over layer-0 links
    // (== size() for a fully connected graph). Diagnostic, O(N).
    size_t reachable_count() const;

private:
    const GameStateVec& vec(uint32_t i) const noexcept { return data_[i]; }

    uint32_t*       links(uint32_t node, size_t layer) noexcept;
    const uint32_t* links(uint32_t node, size_t layer) const noexcept;

    void   add_node(size_t external_id);
    size_t random_level();

    // Greedy hop through layers [from_layer .. to_layer], returning the closest
    // node found at to_layer.
    uint32_t greedy_descend(const GameStateVec& q, uint32_t entry,
                            size_t from_layer, size_t to_layer) const;

    // Beam search at one layer → up to ef (dist_sq, internal id), ascending.
    std::vector<std::pair<float, uint32_t>>
    search_layer(const GameStateVec& q, uint32_t entry, size_t ef, size_t layer) const;

    // Neighbor-selection heuristic (paper Alg. 4, keepPrunedConnections):
    // keeps candidates that are closer to `base` than to any already-kept
    // neighbor, then backfills with pruned ones up to M. `cands` must be sorted
    // ascending; it is replaced by the selection.
    void select_neighbors(std::vector<std::pair<float, uint32_t>>& cands, size_t M) const;

    // Link `node` to `selected` at `layer` and add pruned reverse links.
    void connect(uint32_t node, size_t layer,
                 const std::vector<std::pair<float, uint32_t>>& selected);

    size_t M_;
    size_t M0_;
    size_t ef_construction_;
    size_t ef_search_;
    double m_L_;

    const GameStateVec*     data_ = nullptr;   // build(): caller's array; insert(): owned_
    std::vector<GameStateVec> owned_;
    std::vector<uint32_t>   external_ids_;
    std::vector<uint8_t>    levels_;
    std::vector<uint32_t>   links0_;
    std::vector<uint32_t>   upper_offset_;
    std::vector<uint32_t>   upper_links_;

    uint32_t     entry_point_ = 0;
    size_t       max_level_   = 0;
    std::mt19937 rng_;
};

} // namespace cortex::analytics
