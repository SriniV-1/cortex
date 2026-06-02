// HNSWIndex — Hierarchical Navigable Small World graph implementation.
//
// Core algorithm follows Malkov & Yashunin (2018):
//   - Multi-layer graph with exponentially decaying level distribution
//   - Greedy descent from top layer, beam search at insertion layer
//   - Simple neighbor selection (keep M closest)
//
// Uses the shared l2_dist_sq() from GameStateIndex.hpp for SIMD-accelerated
// distance computation on ARM NEON (Apple Silicon).

#include "analytics/HNSWIndex.hpp"
#include "common/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_set>

namespace cortex::analytics {

// ── Constructor ─────────────────────────────────────────────────────────────

HNSWIndex::HNSWIndex(size_t M, size_t efConstruction)
    : M_(M)
    , M_max0_(2 * M)
    , ef_construction_(efConstruction)
    , m_L_(1.0 / std::log(static_cast<double>(M)))
    , rng_(42)  // deterministic seed for reproducibility
{}

// ── Level generation ────────────────────────────────────────────────────────

size_t HNSWIndex::random_level() {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng_);
    // Avoid log(0)
    if (r < 1e-15) r = 1e-15;
    auto level = static_cast<size_t>(std::floor(-std::log(r) * m_L_));
    // Cap at a reasonable maximum
    return std::min(level, size_t{16});
}

// ── Greedy search — descend from top layer to target layer ──────────────────

size_t HNSWIndex::greedy_search(const GameStateVec& query, size_t entry,
                                 size_t top_layer, size_t target_layer) const {
    size_t cur = entry;
    float cur_dist = l2_dist_sq(query, nodes_[cur].vec);

    for (size_t layer = top_layer; layer > target_layer; --layer) {
        bool improved = true;
        while (improved) {
            improved = false;
            const auto& neighbors = nodes_[cur].neighbors[layer];
            for (size_t neighbor : neighbors) {
                float d = l2_dist_sq(query, nodes_[neighbor].vec);
                if (d < cur_dist) {
                    cur_dist = d;
                    cur = neighbor;
                    improved = true;
                }
            }
        }
    }
    return cur;
}

// ── Beam search at a single layer ───────────────────────────────────────────

std::vector<std::pair<float, size_t>>
HNSWIndex::search_layer(const GameStateVec& query, size_t entry,
                          size_t ef, size_t layer) const {
    // candidates: min-heap (closest first) for expansion
    // results:    max-heap (furthest first) for pruning
    using Entry = std::pair<float, size_t>;

    float entry_dist = l2_dist_sq(query, nodes_[entry].vec);

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> candidates;
    std::priority_queue<Entry> results;
    std::unordered_set<size_t> visited;

    candidates.push({entry_dist, entry});
    results.push({entry_dist, entry});
    visited.insert(entry);

    while (!candidates.empty()) {
        auto [c_dist, c_id] = candidates.top();
        float furthest_dist = results.top().first;

        // If the closest candidate is further than the furthest result, stop.
        if (c_dist > furthest_dist) break;
        candidates.pop();

        // Expand neighbors at this layer.
        const auto& neighbors = nodes_[c_id].neighbors[layer];
        for (size_t neighbor : neighbors) {
            if (visited.count(neighbor)) continue;
            visited.insert(neighbor);

            float d = l2_dist_sq(query, nodes_[neighbor].vec);
            furthest_dist = results.top().first;

            if (d < furthest_dist || results.size() < ef) {
                candidates.push({d, neighbor});
                results.push({d, neighbor});
                if (results.size() > ef) results.pop();
            }
        }
    }

    // Extract results sorted by distance ascending.
    std::vector<Entry> result_vec;
    result_vec.reserve(results.size());
    while (!results.empty()) {
        result_vec.push_back(results.top());
        results.pop();
    }
    std::reverse(result_vec.begin(), result_vec.end());
    return result_vec;
}

// ── Neighbor selection (simple: keep M closest) ─────────────────────────────

std::vector<size_t>
HNSWIndex::select_neighbors(const std::vector<std::pair<float, size_t>>& candidates,
                              size_t M) {
    // candidates are already sorted ascending by distance from search_layer.
    std::vector<size_t> selected;
    selected.reserve(std::min(candidates.size(), M));
    for (size_t i = 0; i < std::min(candidates.size(), M); ++i) {
        selected.push_back(candidates[i].second);
    }
    return selected;
}

// ── Shrink neighbor list to max_M ───────────────────────────────────────────

void HNSWIndex::shrink_neighbors(size_t node_id, size_t layer, size_t max_M) {
    auto& nbrs = nodes_[node_id].neighbors[layer];
    if (nbrs.size() <= max_M) return;

    // Re-rank by distance and keep closest max_M.
    const auto& vec = nodes_[node_id].vec;
    std::vector<std::pair<float, size_t>> scored;
    scored.reserve(nbrs.size());
    for (size_t n : nbrs) {
        scored.push_back({l2_dist_sq(vec, nodes_[n].vec), n});
    }
    std::sort(scored.begin(), scored.end());

    nbrs.clear();
    nbrs.reserve(max_M);
    for (size_t i = 0; i < max_M && i < scored.size(); ++i) {
        nbrs.push_back(scored[i].second);
    }
}

// ── Insert ──────────────────────────────────────────────────────────────────

void HNSWIndex::insert(size_t id, const GameStateVec& vec) {
    std::lock_guard<std::mutex> lock(build_mutex_);

    size_t level = random_level();
    size_t internal_id = nodes_.size();

    Node node;
    node.vec = vec;
    node.external_id = id;
    node.level = level;
    node.neighbors.resize(level + 1);
    nodes_.push_back(std::move(node));

    if (num_elements_ == 0) {
        // First element — just set as entry point.
        entry_point_ = internal_id;
        max_level_ = level;
        ++num_elements_;
        return;
    }

    // Phase 1: Greedy descent from top layer down to (level + 1).
    size_t cur_entry = entry_point_;
    if (max_level_ > level) {
        cur_entry = greedy_search(vec, entry_point_, max_level_, level + 1);
    }

    // Phase 2: Insert at layers [min(level, max_level_) ... 0].
    size_t insert_top = std::min(level, max_level_);
    for (size_t lc = 0; lc <= insert_top; ++lc) {
        size_t actual_layer = insert_top - lc;  // iterate top-down

        auto candidates = search_layer(vec, cur_entry, ef_construction_, actual_layer);
        size_t max_M = (actual_layer == 0) ? M_max0_ : M_;
        auto neighbors = select_neighbors(candidates, max_M);

        // Set this node's neighbors.
        nodes_[internal_id].neighbors[actual_layer] = neighbors;

        // Add bidirectional connections.
        for (size_t neighbor : neighbors) {
            nodes_[neighbor].neighbors[actual_layer].push_back(internal_id);
            // Prune if over capacity.
            shrink_neighbors(neighbor, actual_layer, max_M);
        }

        // Update entry for next layer down.
        if (!candidates.empty()) {
            cur_entry = candidates[0].second;
        }
    }

    // If the new node has a higher level, it becomes the entry point.
    if (level > max_level_) {
        entry_point_ = internal_id;
        max_level_ = level;
    }

    ++num_elements_;
}

// ── Search ──────────────────────────────────────────────────────────────────

std::vector<std::pair<float, size_t>>
HNSWIndex::search(const GameStateVec& query, size_t k, size_t ef) const {
    if (num_elements_ == 0) return {};
    if (ef == 0) ef = std::max(k, ef_construction_);
    k = std::min(k, num_elements_);

    // Greedy descent from top layer to layer 1.
    size_t entry = entry_point_;
    if (max_level_ > 0) {
        entry = greedy_search(query, entry_point_, max_level_, 1);
    }

    // Beam search at layer 0.
    auto candidates = search_layer(query, entry, ef, 0);

    // Return top-k with external ids.
    std::vector<std::pair<float, size_t>> results;
    results.reserve(std::min(k, candidates.size()));
    for (size_t i = 0; i < std::min(k, candidates.size()); ++i) {
        auto& [dist, internal_id] = candidates[i];
        results.push_back({dist, nodes_[internal_id].external_id});
    }
    return results;
}

// ── Batch build ─────────────────────────────────────────────────────────────

void HNSWIndex::build(const std::vector<GameStateVec>& vectors) {
    auto log = cortex::get_logger("hnsw");
    log->info("HNSW: building index over {} vectors (M={}, efConstruction={})",
              vectors.size(), M_, ef_construction_);

    nodes_.reserve(vectors.size());

    for (size_t i = 0; i < vectors.size(); ++i) {
        insert(i, vectors[i]);

        if (i > 0 && i % 100'000 == 0) {
            log->info("HNSW: inserted {}K / {}K vectors, max_level={}",
                      i / 1000, vectors.size() / 1000, max_level_);
        }
    }

    log->info("HNSW: build complete — {} nodes, max_level={}", num_elements_, max_level_);
}

} // namespace cortex::analytics
