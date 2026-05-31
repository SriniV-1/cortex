#pragma once
// EloTracker — computes and stores team Elo ratings from game results.
//
// Standard Elo with K=20 for regular season, K=32 for playoffs.
// Home court advantage: +100 Elo equivalent (~60% expected win rate).
// Ratings carry across seasons with regression to mean (shrink 25% toward 1500).
//
// Usage:
//   EloTracker elo;
//   elo.build_from_db(conn);               // compute from all historical games
//   float rating = elo.rating(team_id);    // current rating for a team
//   auto all = elo.all_ratings();          // snapshot of all team ratings

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <shared_mutex>

namespace pqxx { class connection; }

namespace cortex::analytics {

struct TeamElo {
    int32_t     team_id;
    std::string tricode;
    float       rating;       // current Elo rating
    int         games_played;
    int         wins;
    int         losses;
};

// Snapshot of a team's rating at the end of a season.
struct EloSnapshot {
    int         season;
    std::string tricode;
    float       rating;
};

class EloTracker {
public:
    static constexpr float INITIAL_RATING   = 1500.0f;
    static constexpr float K_REGULAR        = 20.0f;
    static constexpr float K_PLAYOFF        = 32.0f;
    static constexpr float HOME_ADVANTAGE   = 100.0f;
    static constexpr float SEASON_REGRESS   = 0.25f;  // shrink 25% toward mean

    EloTracker() = default;

    // Compute Elo ratings from all completed games in chronological order.
    // Thread-safe: acquires exclusive lock during build.
    void build_from_db(pqxx::connection& conn);

    // Persist current ratings to the team_elo table.
    void save_to_db(pqxx::connection& conn) const;

    // Get current rating for a team. Returns INITIAL_RATING if unknown.
    float rating(int32_t team_id) const;

    // Get Elo diff: home_elo - away_elo (includes home advantage).
    float elo_diff(int32_t home_team_id, int32_t away_team_id) const;

    // Expected win probability for home team given raw Elo ratings.
    static float expected_score(float home_elo, float away_elo);

    // All current ratings, sorted by rating descending.
    std::vector<TeamElo> all_ratings() const;

    // Elo rating history: end-of-season snapshots for every team.
    // Enables trajectory charts showing how ratings evolved across seasons.
    std::vector<EloSnapshot> elo_history() const;

    bool   built() const noexcept { return built_; }
    size_t games_processed() const noexcept { return games_processed_; }
    double build_ms() const noexcept { return build_ms_; }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<int32_t, TeamElo> ratings_;
    std::vector<EloSnapshot> history_;  // season-end snapshots
    bool   built_ = false;
    size_t games_processed_ = 0;
    double build_ms_ = 0.0;

    void update(int32_t winner_id, int32_t loser_id,
                bool home_won, bool is_playoff);
    void regress_to_mean();
};

} // namespace cortex::analytics
