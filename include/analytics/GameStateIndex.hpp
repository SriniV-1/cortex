#pragma once
// GameStateIndex — SIMD-vectorized game state similarity search.
//
// Encodes every play_event in the corpus as a normalized 8-float feature vector,
// then stores them in an AoS (array-of-structs) layout for a brute-force L2
// scan using ARM NEON intrinsics.
//
// Query: returns top-K nearest historical game states in ~2–8 ms across 3.7M
// events (memory-bandwidth limited, no approximation, no external library).
//
// Memory footprint: ~118 MB for 3.7 M events × 8 floats × 4 bytes.
//
// Thread model:
//   - build_from_db() is called once from a background thread at startup.
//   - query() is safe to call from any thread once loaded() returns true.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pqxx { class connection; }

namespace cortex::analytics {

// ── Feature vector ─────────────────────────────────────────────────────────
// 8 normalized floats describing a game state.
//
//  [0] score_diff       (home − away) / 35, clamped to [−1, 1]
//  [1] time_frac        fraction of regulation elapsed [0, 1]
//  [2] period_frac      fraction of current period elapsed [0, 1]
//  [3] home_score_norm  home_score / 140
//  [4] away_score_norm  away_score / 140
//  [5] momentum         recent (last-10-events) score-diff change / 15
//  [6] pace_norm        total points / 280
//  [7] closeness        1 − min(|diff|, 20) / 20  (1 = tie, 0 = 20-pt game)
struct alignas(32) GameStateVec {
    float v[8];
};
static_assert(sizeof(GameStateVec) == 32, "GameStateVec must be 32 bytes");

// Encode raw game fields into a normalized feature vector.
GameStateVec encode_game_state(int score_home, int score_away,
                                int period,     int clock_secs_remaining,
                                int momentum_10evt) noexcept;

// ── Nearest-neighbor result ─────────────────────────────────────────────────
struct GameStateMatch {
    int64_t     event_id;
    std::string game_id;
    int32_t     score_home;
    int32_t     score_away;
    int8_t      period;
    float       similarity;     // 1 / (1 + sqrt(dist_sq)), higher = more similar
    bool        home_won;
    std::string date;           // game date YYYY-MM-DD
    std::string home_tricode;   // e.g. "MIL"
    std::string away_tricode;   // e.g. "BOS"
};

// ── GameStateIndex ──────────────────────────────────────────────────────────
class GameStateIndex {
public:
    GameStateIndex() = default;
    ~GameStateIndex() = default;

    // Noncopyable — the flat feature array is large.
    GameStateIndex(const GameStateIndex&)            = delete;
    GameStateIndex& operator=(const GameStateIndex&) = delete;
    GameStateIndex(GameStateIndex&&)                 = delete;
    GameStateIndex& operator=(GameStateIndex&&)      = delete;

    // Load all play_events from the database into the in-memory index.
    // Runs the window-function SQL, parses clock strings, encodes features.
    // Sets loaded() = true when complete (uses release semantics).
    // Typically 15–30 s for 3.7 M events.
    void build_from_db(pqxx::connection& conn);

    // Return the K most similar historical game states.
    // Thread-safe after loaded() returns true.
    std::vector<GameStateMatch> query(const GameStateVec& q, int k = 10) const;

    // True once build_from_db() has completed successfully.
    bool   loaded()  const noexcept { return loaded_.load(std::memory_order_acquire); }
    size_t size()    const noexcept { return N_; }

    // Milliseconds taken to build (set at end of build_from_db).
    double build_ms() const noexcept { return build_ms_; }

private:
    // AoS feature storage: vecs_[i].v[0..7] = 8 features for event i.
    // 32-byte alignment enables 2-NEON-load L2 distance computation.
    std::vector<GameStateVec> vecs_;

    // Parallel metadata arrays (indexed by same i as vecs_).
    std::vector<int64_t>      event_ids_;
    std::vector<int32_t>      score_homes_;
    std::vector<int32_t>      score_aways_;
    std::vector<int8_t>       periods_;
    std::vector<uint8_t>      home_wons_;      // 1 = home team won
    std::vector<std::string>  game_ids_;
    std::vector<std::string>  dates_;
    std::vector<std::string>  home_tricodes_;
    std::vector<std::string>  away_tricodes_;

    size_t N_       = 0;
    double build_ms_= 0.0;
    std::atomic<bool> loaded_{false};
};

} // namespace cortex::analytics
