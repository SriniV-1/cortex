// HNSWIndex — Hierarchical Navigable Small World graph implementation.
//
// Follows Malkov & Yashunin (2018): exponentially decaying level distribution,
// greedy descent through upper layers, beam search at the target layer, and
// the neighbor-selection heuristic with pruned-connection backfill (which keeps
// the graph connected when many game states are exact duplicates).
//
// Distances use the shared NEON l2_dist_sq() from GameStateIndex.hpp.

#include "analytics/HNSWIndex.hpp"
#include "common/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <queue>

namespace cortex::analytics {

namespace {

// Generation-stamped visited set, one per thread. A node is visited in the
// current search iff tag[node] == cur, so resetting between searches is O(1).
struct VisitedTags {
    std::vector<uint16_t> tag;
    uint16_t              cur = 0;
};

VisitedTags& visited_for(size_t n) {
    static thread_local VisitedTags vt;
    if (vt.tag.size() < n) vt.tag.resize(std::max(n * 2, size_t{1024}), 0);
    if (++vt.cur == 0) {                       // wrapped: clear stale stamps
        std::fill(vt.tag.begin(), vt.tag.end(), 0);
        vt.cur = 1;
    }
    return vt;
}

} // namespace

HNSWIndex::HNSWIndex(size_t M, size_t efConstruction, size_t efSearch)
    : M_(M)
    , M0_(2 * M)
    , ef_construction_(efConstruction)
    , ef_search_(efSearch)
    , m_L_(1.0 / std::log(static_cast<double>(M)))
    , rng_(42)  // deterministic seed for reproducible graphs
{}

size_t HNSWIndex::memory_bytes() const noexcept {
    return owned_.capacity() * sizeof(GameStateVec)
         + external_ids_.capacity() * sizeof(uint32_t)
         + levels_.capacity()
         + links0_.capacity() * sizeof(uint32_t)
         + upper_offset_.capacity() * sizeof(uint32_t)
         + upper_links_.capacity() * sizeof(uint32_t);
}

size_t HNSWIndex::reachable_count() const {
    if (size() == 0) return 0;
    std::vector<bool> seen(size(), false);
    std::vector<uint32_t> stack{entry_point_};
    seen[entry_point_] = true;
    size_t count = 0;
    while (!stack.empty()) {
        const uint32_t node = stack.back();
        stack.pop_back();
        ++count;
        const uint32_t* ln = links(node, 0);
        for (uint32_t j = 1; j <= ln[0]; ++j) {
            if (!seen[ln[j]]) { seen[ln[j]] = true; stack.push_back(ln[j]); }
        }
    }
    return count;
}

uint32_t* HNSWIndex::links(uint32_t node, size_t layer) noexcept {
    if (layer == 0) return &links0_[static_cast<size_t>(node) * (M0_ + 1)];
    return &upper_links_[upper_offset_[node] + (layer - 1) * (M_ + 1)];
}

const uint32_t* HNSWIndex::links(uint32_t node, size_t layer) const noexcept {
    if (layer == 0) return &links0_[static_cast<size_t>(node) * (M0_ + 1)];
    return &upper_links_[upper_offset_[node] + (layer - 1) * (M_ + 1)];
}

size_t HNSWIndex::random_level() {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng_);
    if (r < 1e-15) r = 1e-15;
    auto level = static_cast<size_t>(std::floor(-std::log(r) * m_L_));
    return std::min(level, size_t{16});
}

uint32_t HNSWIndex::greedy_descend(const GameStateVec& q, uint32_t entry,
                                   size_t from_layer, size_t to_layer) const {
    uint32_t cur = entry;
    float cur_dist = l2_dist_sq(q, vec(cur));
    for (size_t layer = from_layer; ; --layer) {
        bool improved = true;
        while (improved) {
            improved = false;
            const uint32_t* ln = links(cur, layer);
            for (uint32_t j = 1; j <= ln[0]; ++j) {
                const float d = l2_dist_sq(q, vec(ln[j]));
                if (d < cur_dist) { cur_dist = d; cur = ln[j]; improved = true; }
            }
        }
        if (layer == to_layer) break;
    }
    return cur;
}

std::vector<std::pair<float, uint32_t>>
HNSWIndex::search_layer(const GameStateVec& q, uint32_t entry, size_t ef, size_t layer) const {
    using Entry = std::pair<float, uint32_t>;
    auto& vis = visited_for(size());

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> candidates;
    std::priority_queue<Entry> best;   // max-heap: worst kept result on top

    const float d0 = l2_dist_sq(q, vec(entry));
    candidates.push({d0, entry});
    best.push({d0, entry});
    vis.tag[entry] = vis.cur;

    while (!candidates.empty()) {
        const auto [c_dist, c_id] = candidates.top();
        if (c_dist > best.top().first && best.size() >= ef) break;
        candidates.pop();

        const uint32_t* ln = links(c_id, layer);
        for (uint32_t j = 1; j <= ln[0]; ++j) {
            const uint32_t nb = ln[j];
            if (vis.tag[nb] == vis.cur) continue;
            vis.tag[nb] = vis.cur;

            const float d = l2_dist_sq(q, vec(nb));
            if (best.size() < ef || d < best.top().first) {
                candidates.push({d, nb});
                best.push({d, nb});
                if (best.size() > ef) best.pop();
            }
        }
    }

    std::vector<Entry> out(best.size());
    for (size_t i = out.size(); i-- > 0;) { out[i] = best.top(); best.pop(); }
    return out;
}

void HNSWIndex::select_neighbors(std::vector<std::pair<float, uint32_t>>& cands,
                                 size_t M) const {
    if (cands.size() <= M) return;
    std::vector<std::pair<float, uint32_t>> kept, pruned;
    kept.reserve(M);
    for (const auto& c : cands) {
        if (kept.size() >= M) break;
        bool diverse = true;
        for (const auto& s : kept) {
            if (l2_dist_sq(vec(c.second), vec(s.second)) < c.first) { diverse = false; break; }
        }
        (diverse ? kept : pruned).push_back(c);
    }
    for (const auto& p : pruned) {
        if (kept.size() >= M) break;
        kept.push_back(p);
    }
    cands.swap(kept);
}

void HNSWIndex::connect(uint32_t node, size_t layer,
                        const std::vector<std::pair<float, uint32_t>>& selected) {
    const size_t max_links = (layer == 0) ? M0_ : M_;

    uint32_t* own = links(node, layer);
    own[0] = static_cast<uint32_t>(selected.size());
    for (size_t i = 0; i < selected.size(); ++i) own[1 + i] = selected[i].second;

    std::vector<std::pair<float, uint32_t>> merged;
    for (const auto& [_, nb] : selected) {
        uint32_t* nl = links(nb, layer);
        if (nl[0] < max_links) { nl[1 + nl[0]++] = node; continue; }

        // Neighbor is full: re-select among its links plus the new node.
        merged.clear();
        const GameStateVec& base = vec(nb);
        for (uint32_t j = 1; j <= nl[0]; ++j) merged.push_back({l2_dist_sq(base, vec(nl[j])), nl[j]});
        merged.push_back({l2_dist_sq(base, vec(node)), node});
        std::sort(merged.begin(), merged.end());
        select_neighbors(merged, max_links);
        nl[0] = static_cast<uint32_t>(merged.size());
        for (size_t i = 0; i < merged.size(); ++i) nl[1 + i] = merged[i].second;
    }
}

void HNSWIndex::add_node(size_t external_id) {
    const auto id    = static_cast<uint32_t>(levels_.size());
    const size_t lvl = random_level();

    levels_.push_back(static_cast<uint8_t>(lvl));
    external_ids_.push_back(static_cast<uint32_t>(external_id));
    links0_.resize(links0_.size() + M0_ + 1, 0);
    upper_offset_.push_back(static_cast<uint32_t>(upper_links_.size()));
    if (lvl > 0) upper_links_.resize(upper_links_.size() + lvl * (M_ + 1), 0);

    if (id == 0) { entry_point_ = 0; max_level_ = lvl; return; }

    const GameStateVec& q = vec(id);
    uint32_t cur = entry_point_;
    if (max_level_ > lvl) cur = greedy_descend(q, cur, max_level_, lvl + 1);

    for (size_t layer = std::min(lvl, max_level_); ; --layer) {
        auto cands = search_layer(q, cur, ef_construction_, layer);
        cur = cands.front().second;
        select_neighbors(cands, M_);
        connect(id, layer, cands);
        if (layer == 0) break;
    }

    if (lvl > max_level_) { max_level_ = lvl; entry_point_ = id; }
}

void HNSWIndex::insert(size_t id, const GameStateVec& v) {
    owned_.push_back(v);
    data_ = owned_.data();   // push_back may reallocate
    add_node(id);
}

std::vector<std::pair<float, size_t>>
HNSWIndex::search(const GameStateVec& query, size_t k, size_t ef) const {
    if (size() == 0) return {};
    k  = std::min(k, size());
    ef = std::max(k, ef ? ef : ef_search_);

    uint32_t entry = entry_point_;
    if (max_level_ > 0) entry = greedy_descend(query, entry, max_level_, 1);

    const auto cands = search_layer(query, entry, ef, 0);

    std::vector<std::pair<float, size_t>> results;
    results.reserve(std::min(k, cands.size()));
    for (size_t i = 0; i < std::min(k, cands.size()); ++i)
        results.push_back({cands[i].first, external_ids_[cands[i].second]});
    return results;
}

void HNSWIndex::build(const std::vector<GameStateVec>& vectors) {
    auto log = cortex::get_logger("hnsw");
    log->info("HNSW: building over {} vectors (M={}, efConstruction={})",
              vectors.size(), M_, ef_construction_);
    const auto t0 = std::chrono::steady_clock::now();

    data_ = vectors.data();
    levels_.reserve(vectors.size());
    external_ids_.reserve(vectors.size());
    links0_.reserve(vectors.size() * (M0_ + 1));
    upper_offset_.reserve(vectors.size());

    for (size_t i = 0; i < vectors.size(); ++i) {
        add_node(i);
        if (i > 0 && i % 500'000 == 0) {
            const double s = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            log->info("HNSW: {}K / {}K inserted ({:.0f} s)", i / 1000, vectors.size() / 1000, s);
        }
    }

    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    log->info("HNSW: build complete — {} nodes, max_level={}, {:.0f} MB, {:.1f} s",
              size(), max_level_, memory_bytes() / (1024.0 * 1024.0), s);
}

} // namespace cortex::analytics
