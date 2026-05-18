#pragma once
// NBAClient — fetches NBA data from the S3-backed public CDN.
//
// Confirmed working endpoint (2026-05-13):
//   https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData/
//
// stats.nba.com is Akamai-blocked; cdn.nba.com is also blocked from most CI/dev
// environments. The S3 bucket requires no auth and responds with clean JSON.
//
// S3 data availability: 2019-present. Pre-2019 returns 403.

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace cortex::etl {

// ── Data structures ────────────────────────────────────────────────────────

struct PlayAction {
    int64_t     action_number;
    std::string clock;           // ISO 8601 duration, e.g. "PT11M45.00S"
    std::string time_actual;     // UTC timestamp, e.g. "2023-11-03T23:11:09.0Z"
    int         period;
    std::string period_type;
    std::string action_type;
    std::string sub_type;
    std::string description;
    int32_t     person_id;       // 0 = team event
    int32_t     team_id;         // 0 = no team (not present in play-by-play JSON)
    float       x;
    float       y;
    int16_t     score_home;
    int16_t     score_away;
    int64_t     order_number;
    std::string qualifiers_json; // raw JSON array, stored as JSONB
};

struct GameSummary {
    std::string game_id;
    std::string away_tricode;
    std::string home_tricode;
    int         away_team_id;
    int         home_team_id;
    std::string game_date;       // "YYYY-MM-DD"
    int         status;          // 1=scheduled, 2=live, 3=final
    int         away_score;
    int         home_score;
};

// Full team metadata from the boxscore endpoint.
struct TeamInfo {
    int32_t     team_id;
    std::string tricode;    // e.g. "BOS"
    std::string name;       // e.g. "Celtics"
    std::string city;       // e.g. "Boston"
};

// Single player row from a boxscore roster.
struct PlayerInfo {
    int32_t     person_id;
    std::string first_name;
    std::string last_name;
    int32_t     team_id;
    int16_t     jersey_num;  // -1 if not available
    std::string position;    // "G", "F", "C", "G-F", etc.
};

// Boxscore response: game metadata + full rosters for both teams.
struct BoxScore {
    GameSummary              game;
    TeamInfo                 home_team;
    TeamInfo                 away_team;
    std::vector<PlayerInfo>  home_players;
    std::vector<PlayerInfo>  away_players;
};

struct PlayByPlay {
    std::string              game_id;
    std::vector<PlayAction>  actions;
};

// ── NBAClient ─────────────────────────────────────────────────────────────

class NBAClient {
public:
    explicit NBAClient(int request_delay_ms = 100);
    ~NBAClient();

    // Fetch today's scoreboard. Returns list of game summaries.
    std::vector<GameSummary> fetch_scoreboard() const;

    // Fetch all play-by-play actions for a single game.
    // Returns nullopt on HTTP error or missing game.
    std::optional<PlayByPlay> fetch_play_by_play(const std::string& game_id) const;

    // Fetch boxscore for a single game: home/away team metadata + player rosters.
    // Returns nullopt on HTTP error or missing game.
    // Note: S3 only has boxscore data for 2019-present seasons.
    std::optional<BoxScore> fetch_boxscore(const std::string& game_id) const;

    // Derive game IDs for a full season by iterating the known counter range.
    // Season format: 2023 → game IDs "0022300001" through "0022301230"
    // (Regular season only. Encodes: 00=league, 2=regular, 23=season, NNNNN=counter)
    static std::vector<std::string> game_ids_for_season(int season, int count = 1230);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int request_delay_ms_;

    std::string http_get(const std::string& url) const;
};

} // namespace cortex::etl
