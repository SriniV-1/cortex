#pragma once
// BulkInserter — batches play events into PostgreSQL via COPY protocol.
// Uses libpqxx's stream_to for maximum throughput (~50K rows/sec on local PG).

#include "NBAClient.hpp"
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include <cstdint>

namespace cortex::etl {

struct InsertStats {
    int64_t games_inserted   = 0;
    int64_t events_inserted  = 0;
    int64_t games_skipped    = 0;  // already in etl_progress with status=done
    int64_t errors           = 0;
};

class BulkInserter {
public:
    // conn_str: libpqxx connection string, e.g. "host=localhost port=5433 dbname=cortex"
    explicit BulkInserter(const std::string& conn_str);
    ~BulkInserter();

    // Upsert team row. Must be called before upsert_game for FK integrity.
    void ensure_team(const TeamInfo& team);

    // Upsert game row. game.game_date must be set (use boxscore, not CURRENT_DATE).
    // Idempotent (ON CONFLICT DO NOTHING).
    void upsert_game(const GameSummary& game);

    // Upsert player rows. Updates team_id/jersey/position on conflict (players change teams).
    void upsert_players(const std::vector<PlayerInfo>& players);

    // Bulk-insert play events for a game using COPY protocol.
    // Wraps the entire game in a transaction; marks etl_progress on success.
    // Returns number of events inserted (0 if already loaded or empty).
    int64_t bulk_insert_play_by_play(const PlayByPlay& pbp,
                                     int season,
                                     const std::string& season_type = "Regular Season");

    // Check whether a game has already been fully loaded.
    bool is_game_loaded(const std::string& game_id) const;

    const InsertStats& stats() const { return stats_; }

private:
    pqxx::connection conn_;
    InsertStats      stats_;

    // Mark etl_progress for a game (called after successful COPY).
    void mark_progress(const std::string& game_id, int season,
                       int64_t event_count, const std::string& status = "done",
                       const std::string& error_msg = "");
};

} // namespace cortex::etl
