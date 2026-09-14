// GameStateIndex — game state similarity search (HNSW + exact NEON scan).
//
// Core design:
//   1. build_from_db()  — one SQL query with LAG() window function computes
//                         10-event rolling momentum per game; rows fetched via
//                         server-side cursor in 50 K-row batches.
//   2. encode_game_state() — maps raw fields → normalized 8-float vector.
//   3. finish_build()   — exposes the exact scan, then builds the HNSW graph
//                         over the same vectors (no copy) and switches query()
//                         to it once ready.
//   4. query_exact()    — brute-force AoS L2 scan; NEON path (Apple Silicon /
//                         ARMv8) uses vld1q_f32 + vfmaq_f32 + vaddvq_f32;
//                         scalar fallback for x86; size-K max-heap with a
//                         pruning threshold.

#include "analytics/GameStateIndex.hpp"
#include "analytics/HNSWIndex.hpp"
#include "common/Logger.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <utility>

namespace cortex::analytics {

GameStateIndex::GameStateIndex() = default;
GameStateIndex::~GameStateIndex() = default;

// ── Feature encoding ────────────────────────────────────────────────────────

GameStateVec encode_game_state(int score_home, int score_away,
                                int period,     int clock_secs_remaining,
                                int momentum_10evt) noexcept {
    GameStateVec g{};

    // [0] Score differential, normalized to [-1, 1]
    const float diff = static_cast<float>(score_home - score_away);
    g.v[0] = std::max(-1.0f, std::min(1.0f, diff / 35.0f));

    // [1] Fraction of regulation time elapsed [0, 1]
    //     Clamp period to [1, 4] to ignore OT for index purposes.
    const int p    = std::max(1, std::min(period, 4));
    const int crem = std::max(0, std::min(720, clock_secs_remaining));
    const int elapsed = (p - 1) * 720 + (720 - crem);
    g.v[1] = static_cast<float>(elapsed) / 2880.0f;

    // [2] Fraction of current period elapsed [0, 1]
    g.v[2] = static_cast<float>(720 - crem) / 720.0f;

    // [3] Home score normalized
    g.v[3] = std::min(static_cast<float>(score_home) / 140.0f, 1.0f);

    // [4] Away score normalized
    g.v[4] = std::min(static_cast<float>(score_away) / 140.0f, 1.0f);

    // [5] Recent momentum: score_diff change over last 10 events, normalized
    g.v[5] = std::max(-1.0f, std::min(1.0f,
                 static_cast<float>(momentum_10evt) / 15.0f));

    // [6] Total pace: combined points normalized
    g.v[6] = std::min(static_cast<float>(score_home + score_away) / 280.0f, 1.0f);

    // [7] Closeness: 1 = tied, 0 = 20+ point game
    const int abs_diff = std::abs(score_home - score_away);
    g.v[7] = 1.0f - static_cast<float>(std::min(abs_diff, 20)) / 20.0f;

    return g;
}

// ── Clock parser ────────────────────────────────────────────────────────────
// Parses ISO 8601 durations like "PT11M45.00S" → seconds remaining.
// Returns 360 (half-period) on parse failure.

static int parse_clock(const std::string& s) noexcept {
    if (s.size() < 3 || s[0] != 'P' || s[1] != 'T') return 360;
    int mins = 0, secs = 0;
    size_t i = 2;
    while (i < s.size() && s[i] != 'M' && s[i] != 'S') {
        if (s[i] >= '0' && s[i] <= '9') mins = mins * 10 + (s[i] - '0');
        ++i;
    }
    if (i < s.size() && s[i] == 'M') ++i;
    // Read seconds (integer part only — ignore fractional)
    while (i < s.size() && s[i] != '.' && s[i] != 'S') {
        if (s[i] >= '0' && s[i] <= '9') secs = secs * 10 + (s[i] - '0');
        ++i;
    }
    return std::max(0, std::min(720, mins * 60 + secs));
}

// ── build_from_db ───────────────────────────────────────────────────────────

void GameStateIndex::build_from_db(pqxx::connection& conn) {
    auto log = cortex::get_logger("similarity");
    log->info("GameStateIndex: starting build from database…");

    const auto t0 = std::chrono::steady_clock::now();

    // SQL: one row per scored event with rolling 10-event momentum.
    // LAG with default = score_diff at first event, so momentum = 0 at start.
    static const char* kSQL = R"SQL(
        SELECT
            pe.event_id,
            pe.game_id,
            pe.score_home,
            pe.score_away,
            pe.period,
            COALESCE(pe.clock, 'PT06M00.00S') AS clock,
            CAST(
                (pe.score_home - pe.score_away)
                - LAG(pe.score_home - pe.score_away,
                      10,
                      pe.score_home - pe.score_away)
                  OVER (PARTITION BY pe.game_id ORDER BY pe.event_id)
            AS INTEGER) AS momentum,
            CASE WHEN g.home_score > g.away_score THEN 1 ELSE 0 END AS home_won,
            g.game_date::text,
            ht.tricode,
            at.tricode
        FROM play_events pe
        JOIN games  g  ON g.game_id    = pe.game_id
        JOIN teams  ht ON ht.team_id   = g.home_team_id
        JOIN teams  at ON at.team_id   = g.away_team_id
        WHERE pe.score_home IS NOT NULL
          AND pe.score_away IS NOT NULL
          AND pe.period BETWEEN 1 AND 4
        ORDER BY pe.game_id, pe.event_id
    )SQL";

    // Pre-reserve for ~4 M events to avoid repeated reallocations.
    constexpr size_t RESERVE = 4'000'000;
    vecs_.reserve(RESERVE);
    event_ids_.reserve(RESERVE);
    score_homes_.reserve(RESERVE);
    score_aways_.reserve(RESERVE);
    periods_.reserve(RESERVE);
    home_wons_.reserve(RESERVE);
    game_ids_.reserve(RESERVE);
    dates_.reserve(RESERVE);
    home_tricodes_.reserve(RESERVE);
    away_tricodes_.reserve(RESERVE);

    try {
        pqxx::work txn(conn);

        // Server-side cursor lets us stream 50 K rows at a time.
        txn.exec("DECLARE sim_cursor CURSOR FOR " + std::string(kSQL));

        size_t total = 0;
        while (true) {
            auto r = txn.exec("FETCH FORWARD 50000 FROM sim_cursor");
            if (r.empty()) break;

            for (const auto& row : r) {
                const int64_t eid   = row[0].as<int64_t>();
                const auto    gid   = row[1].as<std::string>();
                const int     sh    = row[2].as<int>(0);
                const int     sa    = row[3].as<int>(0);
                const int     per   = row[4].as<int>(1);
                const auto    clk   = row[5].as<std::string>("PT06M00.00S");
                const int     mom   = row[6].as<int>(0);
                const int     hw    = row[7].as<int>(0);
                const auto    date  = row[8].as<std::string>("");
                const auto    htri  = row[9].as<std::string>("???");
                const auto    atri  = row[10].as<std::string>("???");

                const int clock_rem = parse_clock(clk);
                const auto vec = encode_game_state(sh, sa, per, clock_rem, mom);

                vecs_.push_back(vec);
                event_ids_.push_back(eid);
                score_homes_.push_back(static_cast<int32_t>(sh));
                score_aways_.push_back(static_cast<int32_t>(sa));
                periods_.push_back(static_cast<int8_t>(per));
                home_wons_.push_back(static_cast<uint8_t>(hw));
                game_ids_.push_back(gid);
                dates_.push_back(date);
                home_tricodes_.push_back(htri);
                away_tricodes_.push_back(atri);

                ++total;
            }

            if (total % 500'000 == 0)
                log->info("GameStateIndex: loaded {}K events…", total / 1000);
        }

        txn.exec("CLOSE sim_cursor");
        txn.commit();

        N_ = total;

        // Trim excess capacity.
        vecs_.shrink_to_fit();
        event_ids_.shrink_to_fit();
        score_homes_.shrink_to_fit();
        score_aways_.shrink_to_fit();
        periods_.shrink_to_fit();
        home_wons_.shrink_to_fit();
        game_ids_.shrink_to_fit();
        dates_.shrink_to_fit();
        home_tricodes_.shrink_to_fit();
        away_tricodes_.shrink_to_fit();

    } catch (const std::exception& e) {
        log->error("GameStateIndex build failed: {}", e.what());
        return;
    }

    const auto t1  = std::chrono::steady_clock::now();
    build_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const double mb = static_cast<double>(N_) * sizeof(GameStateVec) / (1024.0 * 1024.0);
    log->info("GameStateIndex: {} events loaded in {:.1f} s, {:.0f} MB feature store",
              N_, build_ms_ / 1000.0, mb);

    finish_build();
}

// ── build_from_vectors ──────────────────────────────────────────────────────

void GameStateIndex::build_from_vectors(std::vector<GameStateVec> vecs) {
    N_    = vecs.size();
    vecs_ = std::move(vecs);
    event_ids_.resize(N_);
    for (size_t i = 0; i < N_; ++i) event_ids_[i] = static_cast<int64_t>(i);
    score_homes_.assign(N_, 0);
    score_aways_.assign(N_, 0);
    periods_.assign(N_, 0);
    home_wons_.assign(N_, 0);
    game_ids_.assign(N_, {});
    dates_.assign(N_, {});
    home_tricodes_.assign(N_, {});
    away_tricodes_.assign(N_, {});
    finish_build();
}

// ── finish_build — expose exact scan, then build the HNSW graph ─────────────

void GameStateIndex::finish_build() {
    // The exact NEON scan can serve as soon as vectors are in memory.
    loaded_.store(true, std::memory_order_release);

    if (similarity_backend_ != "hnsw" || N_ == 0) return;

    auto log = cortex::get_logger("similarity");
    const auto t0 = std::chrono::steady_clock::now();

    // Collapse identical states: sort event indices by vector bytes (ties by
    // index for determinism), then emit one unique vector per run.
    std::vector<uint32_t> order(N_);
    std::iota(order.begin(), order.end(), uint32_t{0});
    std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
        const int c = std::memcmp(vecs_[a].v, vecs_[b].v, sizeof(vecs_[a].v));
        return c < 0 || (c == 0 && a < b);
    });

    unique_vecs_.clear();
    unique_offsets_.clear();
    for (size_t i = 0; i < N_; ++i) {
        if (i == 0 || std::memcmp(vecs_[order[i]].v, vecs_[order[i - 1]].v,
                                  sizeof(vecs_[0].v)) != 0) {
            unique_offsets_.push_back(static_cast<uint32_t>(i));
            unique_vecs_.push_back(vecs_[order[i]]);
        }
    }
    unique_offsets_.push_back(static_cast<uint32_t>(N_));
    unique_events_ = std::move(order);

    log->info("GameStateIndex: {} events collapse to {} unique game states",
              N_, unique_vecs_.size());

    // Graph parameters; override with CORTEX_HNSW_M / CORTEX_HNSW_EF_CONSTRUCTION.
    // Empty, non-numeric or zero values fall back to the default (a zero beam
    // width would silently build a useless graph).
    auto env_size = [](const char* name, size_t fallback) {
        const char* v = std::getenv(name);
        const unsigned long parsed = v ? std::strtoul(v, nullptr, 10) : 0;
        return parsed > 0 ? static_cast<size_t>(parsed) : fallback;
    };
    // Defaults chosen on the 4.7M-event corpus: M=16/efC=200 gives 0.978
    // recall@10 at ef=64 on realistic game states with a 402 MB graph.
    const size_t M   = env_size("CORTEX_HNSW_M", 16);
    const size_t efc = env_size("CORTEX_HNSW_EF_CONSTRUCTION", 200);

    auto graph = std::make_unique<HNSWIndex>(M, efc);
    graph->build(unique_vecs_);
    hnsw_index_ = std::move(graph);

    hnsw_build_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    log->info("GameStateIndex: HNSW graph serving queries ({} unique states, {:.1f} s)",
              unique_vecs_.size(), hnsw_build_ms_ / 1000.0);
    hnsw_ready_.store(true, std::memory_order_release);
}

// ── set_similarity_backend ──────────────────────────────────────────────────

void GameStateIndex::set_similarity_backend(const std::string& backend) {
    if (backend != "brute_force" && backend != "hnsw") {
        throw std::invalid_argument(
            "Unknown similarity_backend: '" + backend + "' (expected 'brute_force' or 'hnsw')");
    }
    similarity_backend_ = backend;
}

// ── query — HNSW graph once ready, exact NEON scan otherwise ────────────────

std::vector<GameStateMatch> GameStateIndex::query(const GameStateVec& q, int k) const {
    if (!loaded() || N_ == 0) return {};
    k = std::max(1, std::min(k, static_cast<int>(N_)));

    if (!hnsw_ready()) return to_matches(scan_exact(q, k));

    // Nearest unique states, expanded to their events until K results.
    const auto states = hnsw_index_->search(q, static_cast<size_t>(k), hnsw_ef_search());
    std::vector<std::pair<float, size_t>> nearest;
    nearest.reserve(static_cast<size_t>(k));
    for (const auto& [dist_sq, uid] : states) {
        for (uint32_t j = unique_offsets_[uid];
             j < unique_offsets_[uid + 1] && nearest.size() < static_cast<size_t>(k); ++j)
            nearest.push_back({dist_sq, unique_events_[j]});
        if (nearest.size() >= static_cast<size_t>(k)) break;
    }
    return to_matches(nearest);
}

std::vector<GameStateMatch> GameStateIndex::query_exact(const GameStateVec& q, int k) const {
    if (!loaded() || N_ == 0) return {};
    k = std::max(1, std::min(k, static_cast<int>(N_)));
    return to_matches(scan_exact(q, k));
}

// ── scan_exact — brute-force NEON L2 scan with a size-K max-heap ────────────

std::vector<std::pair<float, size_t>> GameStateIndex::scan_exact(const GameStateVec& q, int k) const {
    using HeapEntry = std::pair<float, size_t>;
    std::priority_queue<HeapEntry> heap;
    float threshold = std::numeric_limits<float>::max();

    for (size_t i = 0; i < N_; ++i) {
        float dist_sq = l2_dist_sq(q, vecs_[i]);

        if (dist_sq < threshold || static_cast<int>(heap.size()) < k) {
            if (static_cast<int>(heap.size()) >= k) heap.pop();
            heap.push({dist_sq, i});
            if (static_cast<int>(heap.size()) >= k)
                threshold = heap.top().first;
        }
    }

    std::vector<HeapEntry> nearest;
    nearest.reserve(heap.size());
    while (!heap.empty()) {
        nearest.push_back(heap.top());
        heap.pop();
    }
    std::reverse(nearest.begin(), nearest.end());
    return nearest;
}

// ── to_matches — (dist_sq, idx) → GameStateMatch ────────────────────────────

std::vector<GameStateMatch> GameStateIndex::to_matches(
        const std::vector<std::pair<float, size_t>>& nearest) const {
    std::vector<GameStateMatch> results;
    results.reserve(nearest.size());

    for (const auto& [dist_sq, idx] : nearest) {
        const float sim = 1.0f / (1.0f + std::sqrt(dist_sq));

        GameStateMatch m;
        m.event_id     = event_ids_[idx];
        m.game_id      = game_ids_[idx];
        m.score_home   = score_homes_[idx];
        m.score_away   = score_aways_[idx];
        m.period       = periods_[idx];
        m.similarity   = sim;
        m.home_won     = home_wons_[idx] != 0;
        m.date         = dates_[idx];
        m.home_tricode = home_tricodes_[idx];
        m.away_tricode = away_tricodes_[idx];

        results.push_back(std::move(m));
    }

    return results;
}

} // namespace cortex::analytics
