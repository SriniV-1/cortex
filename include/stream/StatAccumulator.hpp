#pragma once
// StatAccumulator — per-player and per-team in-memory stat tables.
//
// Updated by the stream processor on every event. Designed for single-writer
// (stream processor thread) multiple-reader (HTTP server threads) access.
// Readers snapshot via atomic loads; no mutex needed for reads.
//
// Stats tracked:
//   - Game totals: points, rebounds, assists, turnovers, fouls
//   - Rolling window: last-N-events for live momentum indicators

#include "StreamEvent.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <deque>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace cortex::stream {

struct PlayerGameStats {
    std::atomic<int32_t> points{0};
    std::atomic<int32_t> rebounds{0};
    std::atomic<int32_t> assists{0};
    std::atomic<int32_t> turnovers{0};
    std::atomic<int32_t> fouls{0};
    std::atomic<int32_t> fga{0};   // field goal attempts
    std::atomic<int32_t> fgm{0};   // field goal makes
    std::atomic<int32_t> fta{0};   // free throw attempts
    std::atomic<int32_t> ftm{0};   // free throw makes

    // Non-copyable due to atomics — must be accessed by pointer or in-place
    PlayerGameStats() = default;
    PlayerGameStats(const PlayerGameStats&) = delete;
    PlayerGameStats& operator=(const PlayerGameStats&) = delete;

    // Thread-safe snapshot for HTTP responses
    struct Snapshot {
        int32_t points, rebounds, assists, turnovers, fouls;
        int32_t fga, fgm, fta, ftm;
        float   fg_pct, ft_pct;
    };

    Snapshot snapshot() const noexcept {
        Snapshot s;
        s.points    = points.load(std::memory_order_acquire);
        s.rebounds  = rebounds.load(std::memory_order_acquire);
        s.assists   = assists.load(std::memory_order_acquire);
        s.turnovers = turnovers.load(std::memory_order_acquire);
        s.fouls     = fouls.load(std::memory_order_acquire);
        s.fga       = fga.load(std::memory_order_acquire);
        s.fgm       = fgm.load(std::memory_order_acquire);
        s.fta       = fta.load(std::memory_order_acquire);
        s.ftm       = ftm.load(std::memory_order_acquire);
        s.fg_pct    = s.fga > 0 ? static_cast<float>(s.fgm) / s.fga : 0.0f;
        s.ft_pct    = s.fta > 0 ? static_cast<float>(s.ftm) / s.fta : 0.0f;
        return s;
    }
};

struct TeamGameStats {
    std::atomic<int32_t> score{0};
    std::atomic<int32_t> rebounds{0};
    std::atomic<int32_t> assists{0};
    std::atomic<int32_t> turnovers{0};
    std::atomic<int32_t> fouls{0};

    TeamGameStats() = default;
    TeamGameStats(const TeamGameStats&) = delete;
    TeamGameStats& operator=(const TeamGameStats&) = delete;
};

// ── StatAccumulator ────────────────────────────────────────────────────────
class StatAccumulator {
public:
    // Process a single event, updating all relevant stat tables.
    void process(const StreamEvent& ev) noexcept;

    // Snapshot player stats for a specific game. Thread-safe.
    PlayerGameStats::Snapshot player_stats(int32_t player_id,
                                           std::string_view game_id) const;

    // Current score for a game. Thread-safe.
    std::pair<int16_t, int16_t> score(std::string_view game_id) const noexcept;

    // Rolling window snapshot — last `window` scoring events for a player.
    // Thread-safe. Returns zeros if player not seen.
    struct RollingSnapshot {
        int32_t window_size;  // actual events captured (may be < requested)
        int32_t points;
        int32_t rebounds;
        int32_t assists;
        int32_t turnovers;
        int32_t fgm;
        int32_t fga;
        float   fg_pct;
        float   momentum;     // points scored in last window / window * 10 (0..10)
    };
    RollingSnapshot rolling_stats(int32_t player_id,
                                  std::string_view game_id,
                                  int window = 20) const;

    // Total events processed
    int64_t event_count() const noexcept {
        return event_count_.load(std::memory_order_relaxed);
    }

    // Evict all in-memory state for a specific game. Call after a game ends
    // to reclaim memory from player_stats_, rolling_log_, team_stats_, scores_.
    void evict_game(std::string_view game_id);

    // Evict entries for all games that have not received an event in the
    // given duration. Returns the number of games evicted.
    size_t evict_stale(std::chrono::seconds max_age);

    void reset() noexcept;

private:
    // game_key = game_id string for map lookup
    struct GameKey {
        char data[12];
        bool operator==(const GameKey& o) const noexcept;
    };
    struct GameKeyHash {
        size_t operator()(const GameKey& k) const noexcept;
    };

    // player_id × game_id → stats
    mutable std::shared_mutex player_mu_;
    std::unordered_map<int64_t, PlayerGameStats> player_stats_;
    // Compound key: player_id * 10^10 + game_id_suffix

    // Rolling event log: last 50 events per player×game for window queries
    static constexpr int kMaxRolling = 50;
    struct RollingEntry { ActionType action; bool shot_made; };
    std::unordered_map<int64_t, std::deque<RollingEntry>> rolling_log_;

    // team_id × game_id → stats
    mutable std::shared_mutex team_mu_;
    std::unordered_map<int64_t, TeamGameStats> team_stats_;

    // Latest score per game (game_id → {home, away})
    mutable std::shared_mutex score_mu_;
    std::unordered_map<std::string, std::pair<int16_t, int16_t>> scores_;

    // Last-event timestamp per game for staleness eviction
    using Clock = std::chrono::steady_clock;
    std::unordered_map<std::string, Clock::time_point> game_last_seen_;  // guarded by score_mu_

    std::atomic<int64_t> event_count_{0};

    // Helpers
    int64_t player_key(int32_t player_id, std::string_view game_id) const noexcept;
    int64_t team_key(int32_t team_id, std::string_view game_id) const noexcept;
};

} // namespace cortex::stream
