// Unit tests for HNSWIndex — approximate nearest-neighbor search.
//
// Tests:
//   1. Empty index returns nothing
//   2. Single element returns itself
//   3. Recall vs brute-force ≥ 95% on synthetic data
//   4. Batch build matches incremental insert
//   5. k > N returns all elements
//   6. Deterministic seed gives reproducible results

#include "analytics/HNSWIndex.hpp"
#include "analytics/GameStateIndex.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_set>
#include <vector>

using namespace cortex::analytics;

// ── Helpers ──────────────────────────────────────────────────────────────────

static GameStateVec make_vec(float v0, float v1, float v2, float v3,
                              float v4, float v5, float v6, float v7) {
    GameStateVec g{};
    g.v[0] = v0; g.v[1] = v1; g.v[2] = v2; g.v[3] = v3;
    g.v[4] = v4; g.v[5] = v5; g.v[6] = v6; g.v[7] = v7;
    return g;
}

static std::vector<GameStateVec> generate_random_vecs(size_t n, uint32_t seed = 123) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<GameStateVec> vecs(n);
    for (auto& v : vecs) {
        for (int i = 0; i < 8; ++i) v.v[i] = dist(rng);
    }
    return vecs;
}

// Brute-force top-k for ground truth.
static std::vector<size_t> brute_force_topk(const std::vector<GameStateVec>& data,
                                             const GameStateVec& query, size_t k) {
    std::vector<std::pair<float, size_t>> dists;
    dists.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        dists.push_back({l2_dist_sq(query, data[i]), i});
    }
    std::sort(dists.begin(), dists.end());
    std::vector<size_t> result;
    for (size_t i = 0; i < std::min(k, dists.size()); ++i) {
        result.push_back(dists[i].second);
    }
    return result;
}

// ── Tests ────────────────────────────────────────────────────────────────────

TEST(HNSWIndex, EmptyIndexReturnsNothing) {
    HNSWIndex idx(16, 200);
    auto query = make_vec(0, 0, 0, 0, 0, 0, 0, 0);
    auto results = idx.search(query, 10);
    EXPECT_TRUE(results.empty());
    EXPECT_EQ(idx.size(), 0u);
}

TEST(HNSWIndex, SingleElement) {
    HNSWIndex idx(16, 200);
    auto vec = make_vec(0.5f, 0.3f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    idx.insert(42, vec);

    EXPECT_EQ(idx.size(), 1u);

    auto results = idx.search(vec, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].second, 42u);  // external id
    EXPECT_NEAR(results[0].first, 0.0f, 1e-6f);  // exact match → dist 0
}

TEST(HNSWIndex, KLargerThanN) {
    HNSWIndex idx(16, 200);
    auto vecs = generate_random_vecs(5);
    for (size_t i = 0; i < vecs.size(); ++i) {
        idx.insert(i, vecs[i]);
    }
    auto query = make_vec(0, 0, 0, 0, 0, 0, 0, 0);
    auto results = idx.search(query, 100);
    EXPECT_EQ(results.size(), 5u);
}

TEST(HNSWIndex, RecallVsBruteForce) {
    // Build an index with 2000 random vectors, then run 50 queries.
    // HNSW should achieve ≥ 95% recall@10 against brute-force ground truth.
    constexpr size_t N = 2000;
    constexpr size_t K = 10;
    constexpr size_t NUM_QUERIES = 50;

    auto data = generate_random_vecs(N, 42);

    HNSWIndex idx(16, 200);
    idx.build(data);
    EXPECT_EQ(idx.size(), N);

    // Generate queries from a different seed.
    auto queries = generate_random_vecs(NUM_QUERIES, 999);

    size_t total_hits = 0;
    for (const auto& q : queries) {
        auto hnsw_results = idx.search(q, K);
        auto gt = brute_force_topk(data, q, K);

        std::unordered_set<size_t> gt_set(gt.begin(), gt.end());
        for (const auto& [dist, id] : hnsw_results) {
            if (gt_set.count(id)) ++total_hits;
        }
    }

    double recall = static_cast<double>(total_hits) / (NUM_QUERIES * K);
    EXPECT_GE(recall, 0.95) << "HNSW recall@10 = " << recall << " (expected >= 0.95)";
}

TEST(HNSWIndex, ResultsSortedByDistance) {
    auto data = generate_random_vecs(500, 77);
    HNSWIndex idx(16, 200);
    idx.build(data);

    auto query = make_vec(0.1f, -0.2f, 0.3f, 0.0f, 0.5f, -0.1f, 0.0f, 0.2f);
    auto results = idx.search(query, 20);

    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].first, results[i].first)
            << "Results not sorted at index " << i;
    }
}

TEST(HNSWIndex, DeterministicResults) {
    // Same seed in HNSWIndex constructor → same graph → same results.
    auto data = generate_random_vecs(200, 55);
    auto query = make_vec(0, 0, 0, 0, 0, 0, 0, 0);

    HNSWIndex idx1(16, 200);
    idx1.build(data);
    auto r1 = idx1.search(query, 5);

    HNSWIndex idx2(16, 200);
    idx2.build(data);
    auto r2 = idx2.search(query, 5);

    ASSERT_EQ(r1.size(), r2.size());
    for (size_t i = 0; i < r1.size(); ++i) {
        EXPECT_EQ(r1[i].second, r2[i].second);
        EXPECT_FLOAT_EQ(r1[i].first, r2[i].first);
    }
}

TEST(HNSWIndex, MaxLevelReasonable) {
    auto data = generate_random_vecs(1000, 88);
    HNSWIndex idx(16, 200);
    idx.build(data);
    // For M=16, expected max level ~log(N)/log(M) ≈ log(1000)/log(16) ≈ 2.5.
    // Allow up to 8 — the important thing is it's bounded and small.
    EXPECT_LE(idx.max_level(), 8u);
    EXPECT_GE(idx.max_level(), 1u);
}
