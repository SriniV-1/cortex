// bench_similarity.cpp — HNSW vs exact NEON scan over the real game-state corpus.
//
// Usage:
//   ./cortex_similarity [db_conn_str]
//
// What it measures (same 1000 random game states for every configuration):
//   1. Vector load time and HNSW graph build time/memory.
//   2. Exact NEON brute-force scan: p50 / p99 latency.
//   3. HNSW at several ef values: recall@10 vs the exact scan, p50 / p99
//      latency, and mean-latency speedup over the exact scan.
//
// Recall is scored by distance (a result counts if it is no farther than the
// exact 10th neighbor) because many historical game states are identical.
//
// Compile with -O3 on Apple Silicon to get the NEON path.

#include "analytics/GameStateIndex.hpp"
#include "analytics/HNSWIndex.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace cortex::analytics;
using Clock = std::chrono::steady_clock;

namespace {

struct LatencyStats { double p50, p99, mean; };

LatencyStats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[static_cast<size_t>(p * (v.size() - 1))]; };
    return {pct(0.50), pct(0.99), std::accumulate(v.begin(), v.end(), 0.0) / v.size()};
}

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    const std::string db_conn = (argc > 1) ? argv[1] : "host=localhost port=5433 dbname=cortex";
    constexpr int K = 10;
    constexpr int NUM_QUERIES = 1000;

    std::printf("\n=== Cortex Similarity Benchmark: HNSW vs exact NEON scan ===\n");
    std::printf("Database: %s\n\n", db_conn.c_str());

    GameStateIndex index;
    {
        pqxx::connection conn(db_conn);
        const auto t0 = Clock::now();
        index.build_from_db(conn);   // loads vectors, then builds the HNSW graph
        if (!index.loaded() || !index.hnsw_ready()) {
            std::fprintf(stderr, "ERROR: index build failed\n");
            return 1;
        }
        std::printf("Vectors:    %zu game states (%.0f MB) loaded in %.1f s\n",
                    index.size(), index.size() * 32.0 / (1024 * 1024), index.build_ms() / 1000.0);
        std::printf("HNSW graph: built in %.1f s (total %.1f s)\n\n",
                    index.hnsw_build_ms() / 1000.0, ms_since(t0) / 1000.0);
    }

    if (const auto* g = index.hnsw_graph()) {
        std::printf("Graph connectivity: %zu / %zu nodes reachable from entry (layer 0)\n\n",
                    g->reachable_count(), g->size());
    }

    std::mt19937 rng(42);

    // Query set A: uniformly random states (many are impossible game situations).
    std::uniform_int_distribution<int> score(0, 130), period(1, 4), clock(0, 720), mom(-15, 15);
    std::vector<GameStateVec> uniform_q;
    for (int i = 0; i < NUM_QUERIES; ++i)
        uniform_q.push_back(encode_game_state(score(rng), score(rng), period(rng), clock(rng), mom(rng)));

    // Query set B: realistic states — score gap ~N(0, 8), momentum ~N(0, 4),
    // total score consistent with time elapsed.
    std::normal_distribution<double> gap(0.0, 8.0), momn(0.0, 4.0);
    std::vector<GameStateVec> realistic_q;
    for (int i = 0; i < NUM_QUERIES; ++i) {
        const int p = period(rng), c = clock(rng);
        const double elapsed = ((p - 1) * 720 + (720 - c)) / 2880.0;
        const int home = std::max(0, static_cast<int>(elapsed * 112 + gap(rng) / 2));
        const int away = std::max(0, static_cast<int>(home - gap(rng)));
        realistic_q.push_back(encode_game_state(home, away, p, c,
                              std::clamp(static_cast<int>(momn(rng)), -15, 15)));
    }

    auto run_set = [&](const char* label, const std::vector<GameStateVec>& queries) {
        for (int i = 0; i < 50; ++i) { index.query_exact(queries[i], K); index.query(queries[i], K); }

        std::vector<std::vector<GameStateMatch>> truth;
        std::vector<double> exact_lat;
        for (const auto& q : queries) {
            const auto t0 = Clock::now();
            truth.push_back(index.query_exact(q, K));
            exact_lat.push_back(ms_since(t0));
        }
        const auto ex = stats(exact_lat);
        std::printf("── %s ──\n", label);
        std::printf("Exact NEON scan      p50 %7.3f ms   p99 %7.3f ms   mean %7.3f ms\n",
                    ex.p50, ex.p99, ex.mean);
        std::printf("%-8s %-12s %-12s %-12s %-10s\n", "ef", "recall@10", "p50 (ms)", "p99 (ms)", "speedup");
        for (size_t ef : {16, 64, 256, 1024, 4096}) {
            index.set_hnsw_ef_search(ef);
            std::vector<double> lat;
            size_t hits = 0;
            for (size_t i = 0; i < queries.size(); ++i) {
                const auto t0 = Clock::now();
                const auto approx = index.query(queries[i], K);
                lat.push_back(ms_since(t0));
                const float kth = truth[i].back().similarity;
                for (const auto& m : approx) if (m.similarity >= kth - 1e-6f) ++hits;
            }
            const auto hs = stats(lat);
            std::printf("%-8zu %-12.4f %-12.3f %-12.3f %.1fx\n", ef,
                        static_cast<double>(hits) / (queries.size() * K), hs.p50, hs.p99, ex.mean / hs.mean);
        }

        std::printf("\n");
    };

    run_set("Realistic game states", realistic_q);
    run_set("Uniform random states", uniform_q);
    index.set_hnsw_ef_search(64);

    std::printf("\n=== Benchmark complete ===\n\n");
    return 0;
}
