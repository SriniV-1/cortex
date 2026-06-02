#include "serving/handlers/search_handler.hpp"
#include "serving/HttpUtils.hpp"
#include "common/Logger.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>

using json = nlohmann::json;

namespace cortex::serving::handlers {

// -- /api/players/search -----------------------------------------------------

void handle_search_players(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }

    std::string query = url_decode(req.query("q"));

    if (query.empty() || query.size() < 2) {
        res.json(R"({"error":"query 'q' must be at least 2 characters"})", 400);
        return;
    }

    auto log = cortex::get_logger("http");
    try {
        pqxx::work txn(*ctx.db);
        auto r = txn.exec(
            "SELECT p.player_id, p.first_name || ' ' || p.last_name AS name, "
            "       t.tricode AS team, COALESCE(p.position, '') AS position, "
            "       COALESCE(SUM(pgs.points), 0) AS pts, "
            "       COALESCE(SUM(pgs.rebounds), 0) AS reb, "
            "       COALESCE(SUM(pgs.steals), 0) AS stl, "
            "       COALESCE(SUM(pgs.blocks), 0) AS blk, "
            "       COUNT(DISTINCT pgs.game_id) AS games, "
            "       ROUND(COALESCE(SUM(pgs.points), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS ppg, "
            "       ROUND(COALESCE(SUM(pgs.rebounds), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS rpg, "
            "       ROUND(COALESCE(SUM(pgs.steals), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS spg, "
            "       ROUND(COALESCE(SUM(pgs.blocks), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS bpg, "
            "       ROUND(COALESCE(SUM(pgs.fgm), 0)::numeric * 100 / NULLIF(COALESCE(SUM(pgs.fga), 0),0), 1) AS fg_pct, "
            "       ROUND(COALESCE(SUM(pgs.ftm), 0)::numeric * 100 / NULLIF(COALESCE(SUM(pgs.fta), 0),0), 1) AS ft_pct "
            "FROM players p "
            "LEFT JOIN teams t ON t.team_id = p.team_id "
            "LEFT JOIN player_game_stats pgs ON pgs.player_id = p.player_id "
            "WHERE (p.first_name || ' ' || p.last_name) ILIKE '%' || $1 || '%' "
            "GROUP BY p.player_id, name, t.tricode, p.position "
            "ORDER BY COALESCE(SUM(pgs.points), 0) DESC "
            "LIMIT 25",
            pqxx::params{query});
        txn.commit();

        json players = json::array();
        for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
            players.push_back({
                {"player_id", r[i]["player_id"].as<int>()},
                {"name",      r[i]["name"].as<std::string>()},
                {"team",      r[i]["team"].as<std::string>("")},
                {"pos",       r[i]["position"].as<std::string>("")},
                {"games",     r[i]["games"].as<int>(0)},
                {"pts",       r[i]["pts"].as<int>(0)},
                {"reb",       r[i]["reb"].as<int>(0)},
                {"stl",       r[i]["stl"].as<int>(0)},
                {"blk",       r[i]["blk"].as<int>(0)},
                {"ppg",       r[i]["ppg"].as<double>(0.0)},
                {"rpg",       r[i]["rpg"].as<double>(0.0)},
                {"spg",       r[i]["spg"].as<double>(0.0)},
                {"bpg",       r[i]["bpg"].as<double>(0.0)},
                {"fg_pct",    r[i]["fg_pct"].as<double>(0.0)},
                {"ft_pct",    r[i]["ft_pct"].as<double>(0.0)}
            });
        }

        json j = {{"query", query}, {"players", std::move(players)}};
        res.json(j.dump());
    } catch (const std::exception& e) {
        log->error("player search DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

// -- /api/games/search -------------------------------------------------------

void handle_search_games(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }

    std::string team = req.query("team");

    if (team.empty()) {
        res.json(R"json({"error":"query 'team' required (e.g. BOS, LAL)"})json", 400);
        return;
    }

    std::transform(team.begin(), team.end(), team.begin(), ::toupper);
    auto log = cortex::get_logger("http");

    try {
        pqxx::work txn(*ctx.db);
        auto r = txn.exec(
            "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, "
            "       g.status, g.season_type, "
            "       ht.tricode AS home, at.tricode AS away, "
            "       ht.full_name AS home_name, at.full_name AS away_name "
            "FROM games g "
            "JOIN teams ht ON ht.team_id = g.home_team_id "
            "JOIN teams at ON at.team_id = g.away_team_id "
            "WHERE ht.tricode = $1 OR at.tricode = $1 "
            "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 50",
            pqxx::params{team});
        txn.commit();

        json games_arr = json::array();
        for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
            games_arr.push_back({
                {"game_id",     r[i]["game_id"].as<std::string>()},
                {"date",        r[i]["game_date"].as<std::string>()},
                {"home",        r[i]["home"].as<std::string>()},
                {"away",        r[i]["away"].as<std::string>()},
                {"home_name",   r[i]["home_name"].as<std::string>()},
                {"away_name",   r[i]["away_name"].as<std::string>()},
                {"home_score",  r[i]["home_score"].as<int>(0)},
                {"away_score",  r[i]["away_score"].as<int>(0)},
                {"status",      r[i]["status"].as<int>(1)},
                {"season_type", r[i]["season_type"].as<std::string>()}
            });
        }

        json j = {{"team", team}, {"games", std::move(games_arr)}};
        res.json(j.dump());
    } catch (const std::exception& e) {
        log->error("games search DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

// -- /api/events/search ------------------------------------------------------

void handle_search_events(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }

    std::string pid_str    = req.query("player_id");
    std::string action     = req.query("action_type");
    std::string game_id    = req.query("game_id");
    std::string limit_str  = req.query("limit");

    int limit_n = 50;
    if (!limit_str.empty()) {
        try { limit_n = std::max(1, std::min(std::stoi(limit_str), 200)); } catch (...) {}
    }

    if (pid_str.empty() && action.empty() && game_id.empty()) {
        res.json(R"({"error":"provide at least one of: player_id, action_type, game_id"})", 400);
        return;
    }

    std::string sql =
        "SELECT pe.event_id, pe.game_id, pe.action_number, "
        "       pe.occurred_at::text, pe.period, pe.clock, "
        "       pe.action_type, pe.sub_type, pe.description, "
        "       pe.player_id, pe.team_id, pe.score_home, pe.score_away, "
        "       p.first_name || ' ' || p.last_name AS player_name "
        "FROM play_events pe "
        "LEFT JOIN players p ON p.player_id = pe.player_id "
        "WHERE 1=1 ";

    static const std::vector<std::string> valid_actions = {
        "2pt", "3pt", "freethrow", "rebound", "steal", "block",
        "turnover", "foul", "substitution", "timeout", "jumpball",
        "period", "violation", "ejection"
    };

    int pid_val = 0;
    if (!pid_str.empty()) {
        try { pid_val = std::stoi(pid_str); } catch (...) {}
    }

    auto log = cortex::get_logger("http");
    try {
        pqxx::work txn(*ctx.db);
        pqxx::result r;

        if (!pid_str.empty() && !action.empty() && !game_id.empty()) {
            bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
            if (!valid) { res.json(R"({"error":"invalid action_type"})", 400); return; }
            r = txn.exec(
                sql + "AND pe.player_id = $1 AND pe.action_type = $2 AND pe.game_id = $3 "
                "ORDER BY pe.occurred_at DESC LIMIT $4",
                pqxx::params{pid_val, action, game_id, limit_n});
        } else if (!pid_str.empty() && !action.empty()) {
            bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
            if (!valid) { res.json(R"({"error":"invalid action_type"})", 400); return; }
            r = txn.exec(
                sql + "AND pe.player_id = $1 AND pe.action_type = $2 "
                "ORDER BY pe.occurred_at DESC LIMIT $3",
                pqxx::params{pid_val, action, limit_n});
        } else if (!pid_str.empty() && !game_id.empty()) {
            r = txn.exec(
                sql + "AND pe.player_id = $1 AND pe.game_id = $2 "
                "ORDER BY pe.action_number ASC LIMIT $3",
                pqxx::params{pid_val, game_id, limit_n});
        } else if (!game_id.empty() && !action.empty()) {
            bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
            if (!valid) { res.json(R"({"error":"invalid action_type"})", 400); return; }
            r = txn.exec(
                sql + "AND pe.game_id = $1 AND pe.action_type = $2 "
                "ORDER BY pe.action_number ASC LIMIT $3",
                pqxx::params{game_id, action, limit_n});
        } else if (!pid_str.empty()) {
            r = txn.exec(
                sql + "AND pe.player_id = $1 ORDER BY pe.occurred_at DESC LIMIT $2",
                pqxx::params{pid_val, limit_n});
        } else if (!game_id.empty()) {
            r = txn.exec(
                sql + "AND pe.game_id = $1 ORDER BY pe.action_number ASC LIMIT $2",
                pqxx::params{game_id, limit_n});
        } else if (!action.empty()) {
            bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
            if (!valid) { res.json(R"({"error":"invalid action_type"})", 400); return; }
            r = txn.exec(
                sql + "AND pe.action_type = $1 ORDER BY pe.occurred_at DESC LIMIT $2",
                pqxx::params{action, limit_n});
        }
        txn.commit();

        json events = json::array();
        for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
            json ev = {
                {"event_id",    r[i]["event_id"].as<int64_t>()},
                {"game_id",     r[i]["game_id"].as<std::string>()},
                {"action_num",  r[i]["action_number"].as<int>()},
                {"time",        r[i]["occurred_at"].as<std::string>()},
                {"period",      r[i]["period"].as<int>()},
                {"clock",       r[i]["clock"].as<std::string>("")},
                {"action",      r[i]["action_type"].as<std::string>()},
                {"sub_type",    r[i]["sub_type"].as<std::string>("")},
                {"desc",        r[i]["description"].as<std::string>("")},
                {"player_name", r[i]["player_name"].as<std::string>("")},
                {"score_home",  r[i]["score_home"].as<int>(0)},
                {"score_away",  r[i]["score_away"].as<int>(0)}
            };
            // player_id can be null
            if (r[i]["player_id"].is_null()) {
                ev["player_id"] = nullptr;
            } else {
                ev["player_id"] = r[i]["player_id"].as<int>();
            }
            events.push_back(std::move(ev));
        }

        json j = {
            {"count",  static_cast<int>(r.size())},
            {"events", std::move(events)}
        };
        res.json(j.dump());
    } catch (const std::exception& e) {
        log->error("events search DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

} // namespace cortex::serving::handlers
