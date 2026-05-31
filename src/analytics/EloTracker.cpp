#include "analytics/EloTracker.hpp"
#include "common/Logger.hpp"

#include <pqxx/pqxx>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace cortex::analytics {

namespace {
    auto log = cortex::get_logger("elo");
}

float EloTracker::expected_score(float home_elo, float away_elo) {
    // Standard Elo formula with home court advantage baked in.
    float diff = (home_elo + HOME_ADVANTAGE) - away_elo;
    return 1.0f / (1.0f + std::pow(10.0f, -diff / 400.0f));
}

float EloTracker::rating(int32_t team_id) const {
    std::shared_lock lock(mu_);
    auto it = ratings_.find(team_id);
    return (it != ratings_.end()) ? it->second.rating : INITIAL_RATING;
}

float EloTracker::elo_diff(int32_t home_team_id, int32_t away_team_id) const {
    return rating(home_team_id) + HOME_ADVANTAGE - rating(away_team_id);
}

std::vector<TeamElo> EloTracker::all_ratings() const {
    std::shared_lock lock(mu_);
    std::vector<TeamElo> result;
    result.reserve(ratings_.size());
    for (const auto& [id, te] : ratings_)
        result.push_back(te);
    std::sort(result.begin(), result.end(),
              [](const TeamElo& a, const TeamElo& b) { return a.rating > b.rating; });
    return result;
}

void EloTracker::update(int32_t winner_id, int32_t loser_id,
                        bool home_won, bool is_playoff) {
    // Ensure both teams exist in the map.
    for (int32_t tid : {winner_id, loser_id}) {
        if (ratings_.find(tid) == ratings_.end()) {
            ratings_[tid] = TeamElo{tid, "", INITIAL_RATING, 0, 0, 0};
        }
    }

    auto& w = ratings_[winner_id];
    auto& l = ratings_[loser_id];

    float K = is_playoff ? K_PLAYOFF : K_REGULAR;

    // Expected score from the winner's perspective.
    float home_elo = home_won ? w.rating : l.rating;
    float away_elo = home_won ? l.rating : w.rating;
    float E_home = expected_score(home_elo, away_elo);

    // Actual results: home team won = 1.0 for home, 0.0 for away.
    // We need to figure out who is home.
    float E_winner = home_won ? E_home : (1.0f - E_home);

    w.rating += K * (1.0f - E_winner);
    l.rating += K * (0.0f - (1.0f - E_winner));

    w.games_played++;
    l.games_played++;
    w.wins++;
    l.losses++;
}

void EloTracker::regress_to_mean() {
    for (auto& [id, te] : ratings_) {
        te.rating = INITIAL_RATING + (1.0f - SEASON_REGRESS) * (te.rating - INITIAL_RATING);
    }
}

void EloTracker::build_from_db(pqxx::connection& conn) {
    auto t0 = std::chrono::steady_clock::now();
    log->info("Building Elo ratings from game results…");

    std::unique_lock lock(mu_);
    ratings_.clear();
    history_.clear();
    games_processed_ = 0;

    pqxx::work txn(conn);

    // First, populate tricodes.
    auto teams = txn.exec("SELECT team_id, tricode FROM teams");
    for (const auto& row : teams) {
        int32_t tid = row["team_id"].as<int32_t>();
        ratings_[tid] = TeamElo{
            tid, row["tricode"].as<std::string>(), INITIAL_RATING, 0, 0, 0
        };
    }

    // Process all completed games in chronological order.
    // Track season boundaries for regression.
    auto games = txn.exec(
        "SELECT g.game_id, g.season, g.season_type, "
        "       g.home_team_id, g.away_team_id, "
        "       g.home_score, g.away_score "
        "FROM games g "
        "WHERE g.status = 3 AND g.home_score IS NOT NULL "
        "ORDER BY g.game_date ASC, g.game_id ASC"
    );
    txn.commit();

    int prev_season = -1;

    for (const auto& row : games) {
        int season = row["season"].as<int>();

        // Snapshot and regress ratings at season boundaries.
        if (prev_season >= 0 && season != prev_season) {
            // Capture end-of-season snapshot before regression
            for (const auto& [id, te] : ratings_) {
                if (!te.tricode.empty())
                    history_.push_back({prev_season, te.tricode, te.rating});
            }
            regress_to_mean();
            log->debug("Season {} → {}: ratings regressed to mean", prev_season, season);
        }
        prev_season = season;

        int32_t home_id = row["home_team_id"].as<int32_t>();
        int32_t away_id = row["away_team_id"].as<int32_t>();
        int home_score  = row["home_score"].as<int>();
        int away_score  = row["away_score"].as<int>();
        bool is_playoff = (row["season_type"].as<std::string>() == "Playoffs");

        if (home_score == away_score) continue; // skip ties (shouldn't happen in NBA)

        bool home_won = home_score > away_score;
        int32_t winner_id = home_won ? home_id : away_id;
        int32_t loser_id  = home_won ? away_id : home_id;

        update(winner_id, loser_id, home_won, is_playoff);
        games_processed_++;
    }

    // Capture final season snapshot
    if (prev_season >= 0) {
        for (const auto& [id, te] : ratings_) {
            if (!te.tricode.empty())
                history_.push_back({prev_season, te.tricode, te.rating});
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    build_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Log top 10 teams (while we still hold the lock — no re-lock needed).
    std::vector<TeamElo> sorted;
    sorted.reserve(ratings_.size());
    for (const auto& [id, te] : ratings_)
        sorted.push_back(te);
    std::sort(sorted.begin(), sorted.end(),
              [](const TeamElo& a, const TeamElo& b) { return a.rating > b.rating; });

    built_ = true;
    lock.unlock();  // release before logging

    log->info("Elo ratings built from {} games in {:.0f}ms", games_processed_, build_ms_);
    for (size_t i = 0; i < std::min<size_t>(10, sorted.size()); ++i) {
        log->info("  #{} {} — {:.0f} ({}-{})",
                  i + 1, sorted[i].tricode, sorted[i].rating,
                  sorted[i].wins, sorted[i].losses);
    }
}

std::vector<EloSnapshot> EloTracker::elo_history() const {
    std::shared_lock lock(mu_);
    return history_;
}

void EloTracker::save_to_db(pqxx::connection& conn) const {
    std::shared_lock lock(mu_);

    pqxx::work txn(conn);
    txn.exec("CREATE TABLE IF NOT EXISTS team_elo ("
             "  team_id INTEGER PRIMARY KEY REFERENCES teams(team_id), "
             "  tricode CHAR(3) NOT NULL, "
             "  rating REAL NOT NULL, "
             "  games_played INTEGER NOT NULL, "
             "  wins INTEGER NOT NULL, "
             "  losses INTEGER NOT NULL, "
             "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
             ")");

    for (const auto& [id, te] : ratings_) {
        txn.exec(
            "INSERT INTO team_elo (team_id, tricode, rating, games_played, wins, losses) "
            "VALUES ($1, $2, $3, $4, $5, $6) "
            "ON CONFLICT (team_id) DO UPDATE "
            "SET rating=$3, games_played=$4, wins=$5, losses=$6, updated_at=now()",
            pqxx::params{te.team_id, te.tricode, te.rating,
                         te.games_played, te.wins, te.losses}
        );
    }
    txn.commit();
    log->info("Saved {} team Elo ratings to DB", ratings_.size());
}

} // namespace cortex::analytics
