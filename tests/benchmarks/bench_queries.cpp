// CORTEX — Phase 1 Query Latency Benchmark
//
// Measures p50/p95/p99 wall-clock latency for the four query types that
// the Phase 3 HTTP server will serve. Pass criterion: p99 < 20ms for all.
//
// Usage:
//   ./cortex_bench [--conn "host=... dbname=..."] [--iterations N]
//
// Queries exercised:
//   1. game_events   — all play events for a single game (main read path)
//   2. player_season — all events for a player in one season
//   3. game_summary  — per-player action counts for one game (aggregation)
//   4. time_range    — count of events in a 30-day window (partition pruning)

#include "common/Logger.hpp"

#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

static constexpr const char* DEFAULT_CONN = "host=localhost port=5433 dbname=cortex";
static constexpr int         DEFAULT_ITER = 200;
static constexpr double      TARGET_P99_MS = 20.0;

// ── Timing helpers ─────────────────────────────────────────────────────────

using Clock    = std::chrono::steady_clock;
using Duration = std::chrono::duration<double, std::milli>;

struct Stats {
    double p50_ms;
    double p95_ms;
    double p99_ms;
    double min_ms;
    double max_ms;
    double mean_ms;
};

static Stats compute_stats(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    size_t n   = samples.size();
    return {
        .p50_ms  = samples[n * 50 / 100],
        .p95_ms  = samples[n * 95 / 100],
        .p99_ms  = samples[n * 99 / 100],
        .min_ms  = samples.front(),
        .max_ms  = samples.back(),
        .mean_ms = sum / static_cast<double>(n),
    };
}

static void print_result(const std::string& label, const Stats& s, bool passed) {
    const char* status = passed ? "  PASS" : "  FAIL";
    std::cout << std::format(
        "{} {:.<30} p50={:5.2f}ms  p95={:5.2f}ms  p99={:5.2f}ms  "
        "(min={:.2f} max={:.2f} mean={:.2f})\n",
        status, label + " ", s.p50_ms, s.p95_ms, s.p99_ms,
        s.min_ms, s.max_ms, s.mean_ms);
}

// ── Sample IDs from the live database ─────────────────────────────────────

static std::vector<std::string> sample_game_ids(pqxx::connection& conn, int n) {
    pqxx::work txn(conn);
    auto r = txn.exec(std::format(
        "SELECT game_id FROM games ORDER BY random() LIMIT {}", n));
    txn.commit();
    std::vector<std::string> ids;
    for (auto row : r) ids.push_back(row[0].as<std::string>());
    return ids;
}

static std::vector<int> sample_player_ids(pqxx::connection& conn, int n) {
    pqxx::work txn(conn);
    // Only players who actually appear in play_events (have >10 events)
    auto r = txn.exec(std::format(
        "SELECT player_id FROM play_events "
        "WHERE player_id IS NOT NULL "
        "GROUP BY player_id HAVING COUNT(*) > 10 "
        "ORDER BY random() LIMIT {}", n));
    txn.commit();
    std::vector<int> ids;
    for (auto row : r) ids.push_back(row[0].as<int>());
    return ids;
}

// ── Benchmark runners ─────────────────────────────────────────────────────

// Q1: fetch all play events for a single game, ordered by action_number
// Exercises play_events_game_idx + partition pruning
static Stats bench_game_events(pqxx::connection& conn,
                                const std::vector<std::string>& game_ids,
                                int iterations) {
    pqxx::work txn(conn);
    // Prepare once; reuse across iterations
    conn.prepare("q_game_events",
        "SELECT event_id, action_number, occurred_at, action_type, "
        "       sub_type, description, player_id, score_home, score_away "
        "FROM   play_events "
        "WHERE  game_id = $1 "
        "ORDER  BY action_number");

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        const auto& gid = game_ids[i % game_ids.size()];
        auto t0 = Clock::now();
        auto r  = txn.exec(pqxx::prepped{"q_game_events"}, pqxx::params{gid});
        auto t1 = Clock::now();
        samples.push_back(Duration(t1 - t0).count());
        (void)r;
    }
    txn.commit();
    return compute_stats(samples);
}

// Q2: all events for a player during one season (rolling year window)
// Exercises play_events_player_idx
static Stats bench_player_season(pqxx::connection& conn,
                                  const std::vector<int>& player_ids,
                                  int iterations) {
    pqxx::work txn(conn);
    conn.prepare("q_player_season",
        "SELECT event_id, game_id, occurred_at, action_type, sub_type "
        "FROM   play_events "
        "WHERE  player_id = $1 "
        "AND    occurred_at >= $2::timestamptz "
        "AND    occurred_at <  $2::timestamptz + interval '1 year' "
        "ORDER  BY occurred_at");

    // Use 2023-10-01 as representative season start
    const std::string season_start = "2023-10-01";

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        int pid = player_ids[i % player_ids.size()];
        auto t0 = Clock::now();
        auto r  = txn.exec(pqxx::prepped{"q_player_season"}, pqxx::params{pid, season_start});
        auto t1 = Clock::now();
        samples.push_back(Duration(t1 - t0).count());
        (void)r;
    }
    txn.commit();
    return compute_stats(samples);
}

// Q3: per-player action-type counts for one game (aggregation over ~500 rows)
// Represents the box-score aggregation a REST endpoint would run
static Stats bench_game_summary(pqxx::connection& conn,
                                 const std::vector<std::string>& game_ids,
                                 int iterations) {
    pqxx::work txn(conn);
    conn.prepare("q_game_summary",
        "SELECT player_id, action_type, COUNT(*) as cnt "
        "FROM   play_events "
        "WHERE  game_id = $1 "
        "AND    player_id IS NOT NULL "
        "GROUP  BY player_id, action_type "
        "ORDER  BY player_id, cnt DESC");

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        const auto& gid = game_ids[i % game_ids.size()];
        auto t0 = Clock::now();
        auto r  = txn.exec(pqxx::prepped{"q_game_summary"}, pqxx::params{gid});
        auto t1 = Clock::now();
        samples.push_back(Duration(t1 - t0).count());
        (void)r;
    }
    txn.commit();
    return compute_stats(samples);
}

// Q4a: event count in a 30-day window via play_events_daily summary table.
// This is the production query path — O(30 rows) instead of O(120K events).
// The raw play_events scan (Q4b below) is informational only.
static Stats bench_time_range_summary(pqxx::connection& conn, int iterations) {
    pqxx::work txn(conn);
    conn.prepare("q_time_range_summary",
        "SELECT COALESCE(SUM(event_count), 0) "
        "FROM   play_events_daily "
        "WHERE  date >= $1::date "
        "AND    date <  $1::date + interval '30 days'");

    const std::vector<std::string> windows = {
        "2020-01-01", "2021-01-15", "2022-03-01",
        "2023-01-01", "2023-11-01", "2024-02-01",
    };

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        const auto& w = windows[i % windows.size()];
        auto t0 = Clock::now();
        auto r  = txn.exec(pqxx::prepped{"q_time_range_summary"}, pqxx::params{w});
        auto t1 = Clock::now();
        samples.push_back(Duration(t1 - t0).count());
        (void)r;
    }
    txn.commit();
    return compute_stats(samples);
}

// Q4b: raw scan of play_events — informational, not the production path.
// Expected: ~35ms (index-only scan over ~120K rows in the 2020-2024 partition).
static Stats bench_time_range_raw(pqxx::connection& conn, int iterations) {
    pqxx::work txn(conn);
    conn.prepare("q_time_range_raw",
        "SELECT COUNT(*) "
        "FROM   play_events "
        "WHERE  occurred_at >= $1::timestamptz "
        "AND    occurred_at <  $1::timestamptz + interval '30 days'");

    const std::vector<std::string> windows = {
        "2020-01-01", "2021-01-15", "2022-03-01",
        "2023-01-01", "2023-11-01", "2024-02-01",
    };

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        const auto& w = windows[i % windows.size()];
        auto t0 = Clock::now();
        auto r  = txn.exec(pqxx::prepped{"q_time_range_raw"}, pqxx::params{w});
        auto t1 = Clock::now();
        samples.push_back(Duration(t1 - t0).count());
        (void)r;
    }
    txn.commit();
    return compute_stats(samples);
}

// ── main ──────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::warn);  // suppress noise during benchmark

    std::string conn_str = DEFAULT_CONN;
    int iterations = DEFAULT_ITER;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--conn" && i+1 < argc) conn_str   = argv[++i];
        else if (arg == "--iter" && i+1 < argc) iterations = std::stoi(argv[++i]);
        else {
            std::cerr << "Usage: cortex_bench [--conn STR] [--iter N]\n";
            return 1;
        }
    }

    pqxx::connection conn(conn_str);

    std::cout << std::format(
        "\nCORTEX Phase 1 Query Benchmark  ({} iterations, target p99 < {:.0f}ms)\n",
        iterations, TARGET_P99_MS);
    std::cout << std::string(80, '-') << "\n";

    // Warm up shared_buffers: touch all four tables before measuring
    {
        pqxx::work txn(conn);
        txn.exec("SELECT COUNT(*) FROM play_events WHERE occurred_at >= '2023-01-01'");
        txn.exec("SELECT COUNT(*) FROM play_events_daily");
        txn.exec("SELECT COUNT(*) FROM games");
        txn.exec("SELECT COUNT(*) FROM players");
        txn.commit();
    }

    // Sample representative IDs from the live DB
    auto game_ids   = sample_game_ids(conn, std::min(iterations, 200));
    auto player_ids = sample_player_ids(conn, std::min(iterations, 200));

    if (game_ids.empty()) {
        std::cerr << "No games found in DB — run the ETL first.\n";
        return 1;
    }
    if (player_ids.empty()) {
        std::cerr << "No players found in DB — run the ETL first.\n";
        return 1;
    }

    std::cout << std::format("Sampled {} game IDs and {} player IDs\n\n",
                             game_ids.size(), player_ids.size());

    bool all_pass = true;

    {
        auto s = bench_game_events(conn, game_ids, iterations);
        bool ok = s.p99_ms < TARGET_P99_MS;
        all_pass &= ok;
        print_result("game_events (all events for one game)", s, ok);
    }
    {
        auto s = bench_player_season(conn, player_ids, iterations);
        bool ok = s.p99_ms < TARGET_P99_MS;
        all_pass &= ok;
        print_result("player_season (player events, 1 season)", s, ok);
    }
    {
        auto s = bench_game_summary(conn, game_ids, iterations);
        bool ok = s.p99_ms < TARGET_P99_MS;
        all_pass &= ok;
        print_result("game_summary (per-player action counts)", s, ok);
    }
    {
        auto s = bench_time_range_summary(conn, iterations);
        bool ok = s.p99_ms < TARGET_P99_MS;
        all_pass &= ok;
        print_result("time_range/summary (30-day window, daily table)", s, ok);
    }

    // Informational — raw scan is not the production query path
    std::cout << "\n  INFO (not included in pass/fail):\n";
    {
        auto s = bench_time_range_raw(conn, iterations);
        std::cout << std::format(
            "  [raw] {:.<38} p50={:5.2f}ms  p95={:5.2f}ms  p99={:5.2f}ms\n",
            "time_range/raw (play_events index-only scan) ",
            s.p50_ms, s.p95_ms, s.p99_ms);
    }

    std::cout << "\n" << std::string(80, '-') << "\n";
    if (all_pass) {
        std::cout << "  ALL PASS — p99 < " << TARGET_P99_MS << "ms for all queries\n\n";
        return 0;
    } else {
        std::cout << "  FAIL — one or more queries exceed " << TARGET_P99_MS << "ms p99\n\n";
        return 1;
    }
}
