#include "etl/BulkInserter.hpp"
#include "common/Logger.hpp"

#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

#include <format>
#include <chrono>
#include <stdexcept>

namespace cortex::etl {

namespace {
    auto log = cortex::get_logger("bulk_inserter");

    // Derive season start year from NBA game_id.
    // Format: 002YYNNNNN where YY = last 2 digits of season start year.
    // e.g. "0022300001" → YY=23 → 2023, "0021900001" → YY=19 → 2019
    int season_from_game_id(const std::string& game_id) {
        if (game_id.size() < 5) return 2023;
        int yy = std::stoi(game_id.substr(3, 2));
        return (yy < 50) ? (2000 + yy) : (1900 + yy);
    }

    // Parse "PT11M45.00S" style clock + game date into a TIMESTAMPTZ string.
    // For historical loads, time_actual from the JSON is used directly (it's UTC).
    // If time_actual is empty, fall back to game date at midnight UTC.
    std::string resolve_timestamp(const std::string& time_actual,
                                  const std::string& game_date) {
        if (!time_actual.empty()) {
            // time_actual format: "2023-11-03T23:11:09.0Z"
            // PostgreSQL accepts ISO 8601 directly
            return time_actual;
        }
        return game_date + "T00:00:00Z";
    }
} // namespace

BulkInserter::BulkInserter(const std::string& conn_str)
    : conn_(conn_str) {
    log->info("Connected to PostgreSQL: {}", conn_.dbname());
}

BulkInserter::~BulkInserter() = default;

// ── Idempotent team insert ─────────────────────────────────────────────────
void BulkInserter::ensure_team(const TeamInfo& team) {
    pqxx::work txn(conn_);
    // Use full name and city from boxscore. ON CONFLICT DO NOTHING — team metadata
    // rarely changes and we don't want to clobber conference/division if set manually.
    txn.exec(
        "INSERT INTO teams (team_id, tricode, full_name, city) "
        "VALUES ($1, $2, $3, $4) ON CONFLICT (team_id) DO NOTHING",
        pqxx::params{team.team_id, team.tricode,
                     team.name.empty() ? team.tricode : team.name,
                     team.city.empty() ? team.tricode : team.city}
    );
    txn.commit();
}

// ── Game upsert ────────────────────────────────────────────────────────────
void BulkInserter::upsert_game(const GameSummary& game) {
    int season = season_from_game_id(game.game_id);
    // game_date must come from the boxscore (gameTimeUTC), not CURRENT_DATE.
    // Fall back to a season-derived date only if the boxscore didn't provide one.
    std::string game_date = game.game_date.empty()
                          ? std::format("{}-10-01", season)
                          : game.game_date;
    pqxx::work txn(conn_);
    txn.exec(
        "INSERT INTO games "
        "  (game_id, season, season_type, game_date, home_team_id, away_team_id, "
        "   home_score, away_score, status) "
        "VALUES ($1, $2, 'Regular Season', $3::date, $4, $5, $6, $7, $8) "
        "ON CONFLICT (game_id) DO NOTHING",
        pqxx::params{game.game_id, season, game_date,
                     game.home_team_id, game.away_team_id,
                     game.home_score, game.away_score,
                     game.status}
    );
    txn.commit();
}

// ── Player upsert ──────────────────────────────────────────────────────────
void BulkInserter::upsert_players(const std::vector<PlayerInfo>& players) {
    if (players.empty()) return;
    pqxx::work txn(conn_);
    for (const auto& p : players) {
        if (p.person_id <= 0) continue;
        // Update team/jersey/position on conflict — players change teams between seasons.
        txn.exec(
            "INSERT INTO players "
            "  (player_id, first_name, last_name, team_id, jersey_num, position, is_active) "
            "VALUES ($1, $2, $3, $4, $5, $6, true) "
            "ON CONFLICT (player_id) DO UPDATE "
            "  SET team_id    = EXCLUDED.team_id, "
            "      jersey_num = EXCLUDED.jersey_num, "
            "      position   = EXCLUDED.position, "
            "      is_active  = true",
            pqxx::params{
                p.person_id,
                p.first_name, p.last_name,
                p.team_id > 0 ? std::optional<int32_t>(p.team_id) : std::nullopt,
                p.jersey_num >= 0 ? std::optional<int16_t>(p.jersey_num) : std::nullopt,
                p.position.empty() ? std::optional<std::string>{} : std::optional<std::string>(p.position)
            }
        );
    }
    txn.commit();
}

// ── Check if game already loaded ───────────────────────────────────────────
bool BulkInserter::is_game_loaded(const std::string& game_id) const {
    pqxx::work txn(const_cast<pqxx::connection&>(conn_));
    auto r = txn.exec(
        "SELECT 1 FROM etl_progress WHERE game_id = $1 AND status = 'done'",
        pqxx::params{game_id}
    );
    txn.commit();
    return !r.empty();
}

// ── Bulk insert via COPY ───────────────────────────────────────────────────
int64_t BulkInserter::bulk_insert_play_by_play(
        const PlayByPlay& pbp,
        int season,
        const std::string& season_type) {

    if (is_game_loaded(pbp.game_id)) {
        log->debug("Skipping already-loaded game {}", pbp.game_id);
        stats_.games_skipped++;
        return 0;
    }

    if (pbp.actions.empty()) {
        log->warn("No actions for game {}", pbp.game_id);
        mark_progress(pbp.game_id, season, 0, "partial", "no actions");
        return 0;
    }

    int64_t inserted = 0;
    // Derive a fallback date string from season (e.g. 2023 → "2023-10-01")
    std::string fallback_date = std::format("{}-10-01", season);

    try {
        pqxx::work txn(conn_);

        // Stable event_id: pack game counter (digits 3–9 of game_id) × 10000 + action_number.
        // This fits in int64 and is unique across all seasons.
        int64_t game_num = std::stoll(pbp.game_id);

        {
            auto stream = pqxx::stream_to::table(
                txn,
                {"play_events"},
                {"event_id", "game_id", "action_number", "occurred_at",
                 "period", "period_type", "clock", "action_type", "sub_type",
                 "description", "player_id", "team_id", "x", "y",
                 "score_home", "score_away", "order_number", "qualifiers"}
            );

            for (const auto& a : pbp.actions) {
                int64_t event_id = game_num * 10000LL + (a.action_number % 10000);
                std::string ts   = resolve_timestamp(a.time_actual, fallback_date);

                stream << std::make_tuple(
                    event_id,
                    pbp.game_id,
                    static_cast<int>(a.action_number),
                    ts,
                    a.period,
                    a.period_type,
                    a.clock,
                    a.action_type,
                    a.sub_type,
                    a.description,
                    a.person_id > 0 ? std::optional<int32_t>(a.person_id) : std::nullopt,
                    a.team_id   > 0 ? std::optional<int32_t>(a.team_id)   : std::nullopt,
                    a.x != 0.0f     ? std::optional<float>(a.x)           : std::nullopt,
                    a.y != 0.0f     ? std::optional<float>(a.y)           : std::nullopt,
                    a.score_home,
                    a.score_away,
                    a.order_number,
                    a.qualifiers_json
                );
                ++inserted;
            }
            stream.complete();
        }

        // Commit the COPY transaction BEFORE marking progress —
        // libpqxx doesn't allow two concurrent transactions on one connection.
        txn.commit();

        mark_progress(pbp.game_id, season, inserted);
        stats_.events_inserted += inserted;
        stats_.games_inserted++;
        log->info("Loaded game {} — {} events", pbp.game_id, inserted);

    } catch (const std::exception& e) {
        log->error("Failed to insert game {}: {}", pbp.game_id, e.what());
        mark_progress(pbp.game_id, season, 0, "error", e.what());
        stats_.errors++;
        return 0;
    }

    return inserted;
}

// ── ETL progress tracking ──────────────────────────────────────────────────
void BulkInserter::mark_progress(
        const std::string& game_id,
        int season,
        int64_t event_count,
        const std::string& status,
        const std::string& error_msg) {

    try {
        pqxx::work txn(conn_);
        if (error_msg.empty()) {
            txn.exec(
                "INSERT INTO etl_progress (season, game_id, event_count, status) "
                "VALUES ($1, $2, $3, $4) "
                "ON CONFLICT (season, game_id) DO UPDATE "
                "SET status=$4, event_count=$3, fetched_at=now()",
                pqxx::params{season, game_id, static_cast<int>(event_count), status}
            );
        } else {
            txn.exec(
                "INSERT INTO etl_progress (season, game_id, event_count, status, error_msg) "
                "VALUES ($1, $2, $3, $4, $5) "
                "ON CONFLICT (season, game_id) DO UPDATE "
                "SET status=$4, event_count=$3, error_msg=$5, fetched_at=now()",
                pqxx::params{season, game_id, static_cast<int>(event_count), status, error_msg}
            );
        }
        txn.commit();
    } catch (const std::exception& e) {
        log->error("mark_progress failed for {}: {}", game_id, e.what());
    }
}

} // namespace cortex::etl
