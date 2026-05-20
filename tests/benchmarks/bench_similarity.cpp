// bench_similarity.cpp — Benchmarks the SIMD game state similarity index.
//
// Usage:
//   ./cortex_similarity [db_conn_str]
//
// What it measures:
//   1. Index build time (one-time, loads from PostgreSQL).
//   2. 1000 randomized single queries → p50 / p95 / p99 / max latency.
//   3. Effective throughput (queries per second).
//
// Compile with -O3 on Apple Silicon to get NEON path.

#include "analytics/GameStateIndex.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cortex::analytics;
using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::duration<double, std::milli>;

static double elapsed_ms(Clock::time_point a, Clock::time_point b) {
    return Ms(b - a).count();
}

static void print_percentiles(std::vector<double>& v, const char* label) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    auto pct = [&](double p) -> double { return v[static_cast<size_t>(p * (n - 1))]; };

    std::cout << "  " << label << "\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "    p50  = " << pct(0.50) << " ms\n";
    std::cout << "    p95  = " << pct(0.95) << " ms\n";
    std::cout << "    p99  = " << pct(0.99) << " ms\n";
    std::cout << "    max  = " << v.back()  << " ms\n";
    const double mean = std::accumulate(v.begin(), v.end(), 0.0) / n;
    std::cout << "    mean = " << mean      << " ms\n";
    std::cout << "    throughput = "
              << std::setprecision(0)
              << (1000.0 / mean) << " queries/s\n";
}

// Representative game states covering different game situations.
struct GameState { int sh, sa, period, clock, momentum; };

static const GameState kTestStates[] = {
    // Close 4th quarters
    {105, 98, 4, 180, 4},
    {88,  88, 4, 120, 0},
    {112, 109, 4, 60,  3},
    // Mid-game situations
    {55,  48, 2, 400, 7},
    {72,  60, 3, 300, -4},
    {45,  45, 1, 600, 0},
    // Blowout scenarios
    {95,  65, 3, 200, 10},
    {70,  100, 4, 400, -8},
    // OT / buzzer
    {102, 102, 4, 30, 0},
    {78,  75, 4, 10, -2},
};

int main(int argc, char** argv) {
    const std::string db_conn = (argc > 1)
        ? argv[1]
        : "host=localhost port=5433 dbname=cortex";

    std::cout << "\n=== Cortex Similarity Index Benchmark ===\n\n";
    std::cout << "Database: " << db_conn << "\n\n";

    // ── Build ──────────────────────────────────────────────────────────────
    GameStateIndex index;
    {
        std::cout << "Building index from database (this takes 15–30s)...\n";
        std::cout.flush();

        pqxx::connection conn(db_conn);

        const auto t0 = Clock::now();
        index.build_from_db(conn);
        const auto t1 = Clock::now();

        if (!index.loaded()) {
            std::cerr << "ERROR: Index build failed (no events loaded?)\n";
            return 1;
        }

        const double bt = elapsed_ms(t0, t1);
        const double mb = static_cast<double>(index.size()) * 32.0 / (1024.0 * 1024.0);
        std::cout << "  Loaded " << index.size() << " events in "
                  << std::fixed << std::setprecision(1) << (bt / 1000.0) << " s\n";
        std::cout << "  Feature store: " << std::setprecision(0) << mb << " MB\n\n";
    }

    // ── Warm-up (3 passes to get data into L3 cache) ───────────────────────
    std::cout << "Warming cache (3 passes)...\n";
    for (int w = 0; w < 3; ++w) {
        for (const auto& s : kTestStates) {
            auto q = encode_game_state(s.sh, s.sa, s.period, s.clock, s.momentum);
            volatile auto r = index.query(q, 10);
            (void)r;
        }
    }

    // ── Benchmark: deterministic game states (1000 queries) ───────────────
    std::cout << "Benchmarking 1000 queries (deterministic states)...\n";
    std::vector<double> latencies;
    latencies.reserve(1000);

    for (int iter = 0; iter < 100; ++iter) {
        for (const auto& s : kTestStates) {
            auto q  = encode_game_state(s.sh, s.sa, s.period, s.clock, s.momentum);
            const auto t0 = Clock::now();
            auto res = index.query(q, 10);
            const auto t1 = Clock::now();
            latencies.push_back(elapsed_ms(t0, t1));
            (void)res;
        }
    }
    print_percentiles(latencies, "Query latency (1000 queries, k=10)");

    // ── Benchmark: random game states (1000 queries) ──────────────────────
    std::cout << "\nBenchmarking 1000 random game states...\n";
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist_score(0, 130);
    std::uniform_int_distribution<int> dist_period(1, 4);
    std::uniform_int_distribution<int> dist_clock(0, 720);
    std::uniform_int_distribution<int> dist_mom(-15, 15);

    latencies.clear();
    for (int i = 0; i < 1000; ++i) {
        const int sh = dist_score(rng);
        const int sa = dist_score(rng);
        auto q = encode_game_state(sh, sa,
                                   dist_period(rng),
                                   dist_clock(rng),
                                   dist_mom(rng));
        const auto t0 = Clock::now();
        auto res = index.query(q, 10);
        const auto t1 = Clock::now();
        latencies.push_back(elapsed_ms(t0, t1));
        (void)res;
    }
    print_percentiles(latencies, "Query latency (1000 queries, random states, k=10)");

    // ── Show a sample result ───────────────────────────────────────────────
    std::cout << "\n--- Sample: Q4 3:00 remaining, 105–98 (+7 home lead) ---\n";
    auto q = encode_game_state(105, 98, 4, 180, 4);
    auto results = index.query(q, 5);
    std::cout << "  Top-5 similar moments:\n";
    for (const auto& m : results) {
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  #" << std::setw(1) << "  "
                  << m.date << "  "
                  << std::setw(3) << m.away_tricode << " @ " << std::setw(3) << m.home_tricode
                  << "  Q" << static_cast<int>(m.period)
                  << "  " << std::setw(3) << m.score_away << "–" << std::setw(3) << m.score_home
                  << "  sim=" << m.similarity
                  << "  " << (m.home_won ? "home won" : "away won") << "\n";
    }

    std::cout << "\n=== Benchmark complete ===\n\n";
    return 0;
}
