#include "serving/handlers/stats_handler.hpp"
#include "serving/HttpUtils.hpp"
#include "common/Logger.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

using json = nlohmann::json;

namespace cortex::serving::handlers {

// -- /stats/:gameId ----------------------------------------------------------

void handle_game_stats(Request& req, Response& res, ServerContext& ctx) {
    std::string game_id = req.param("gameId");
    if (game_id.empty()) {
        res.json(R"({"error":"missing game_id"})", 400);
        return;
    }

    if (!ctx.accumulator) {
        res.json(R"({"error":"no accumulator"})", 500);
        return;
    }

    // Cache-aside: try Redis first
    const std::string cache_key = "cortex:stats:" + game_id;
    if (ctx.cache) {
        auto cached = ctx.cache->get(cache_key);
        if (cached) {
            res.json(*cached);
            return;
        }
    }

    auto [home, away] = ctx.accumulator->score(game_id);
    json j = {
        {"game_id",          game_id},
        {"score_home",       home},
        {"score_away",       away},
        {"events_processed", ctx.accumulator->event_count()}
    };
    std::string body = j.dump();

    if (ctx.cache) ctx.cache->set(cache_key, body, 60);

    res.json(body);
}

// -- /players/:playerId/season -----------------------------------------------

void handle_player_season(Request& req, Response& res, ServerContext& ctx) {
    std::string pid_str = req.param("playerId");
    int32_t player_id = 0;
    try { player_id = std::stoi(pid_str); }
    catch (...) {
        res.json(R"({"error":"invalid player_id"})", 400);
        return;
    }

    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }

    auto log = cortex::get_logger("http");
    try {
        pqxx::work txn(*ctx.db);
        auto r = txn.exec_params(
            "SELECT SUM(points) AS pts, SUM(rebounds) AS reb, "
            "       SUM(assists) AS ast, SUM(steals) AS stl, "
            "       SUM(blocks) AS blk, SUM(turnovers) AS to_, "
            "       SUM(fga) AS fga, SUM(fgm) AS fgm, "
            "       SUM(fta) AS fta, SUM(ftm) AS ftm, "
            "       COUNT(DISTINCT game_id) AS games "
            "FROM player_game_stats "
            "WHERE player_id = $1",
            player_id);
        txn.commit();

        if (r.empty() || r[0][0].is_null()) {
            res.json(R"({"error":"player not found"})", 404);
            return;
        }
        auto row = r[0];
        json j = {
            {"player_id", player_id},
            {"games",     row["games"].as<int>(0)},
            {"points",    row["pts"].as<int>(0)},
            {"rebounds",  row["reb"].as<int>(0)},
            {"assists",   row["ast"].as<int>(0)},
            {"steals",    row["stl"].as<int>(0)},
            {"blocks",    row["blk"].as<int>(0)},
            {"turnovers", row["to_"].as<int>(0)},
            {"fga",       row["fga"].as<int>(0)},
            {"fgm",       row["fgm"].as<int>(0)},
            {"fta",       row["fta"].as<int>(0)},
            {"ftm",       row["ftm"].as<int>(0)}
        };
        res.json(j.dump());
    } catch (const std::exception& e) {
        log->error("DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

// -- /api/stats --------------------------------------------------------------

void handle_api_stats(Request& /*req*/, Response& res, ServerContext& ctx) {
    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }
    auto log = cortex::get_logger("http");
    try {
        pqxx::work txn(*ctx.db);
        auto r = txn.exec(
            "SELECT "
            "  (SELECT COUNT(*) FROM games)         AS total_games, "
            "  (SELECT COUNT(*) FROM players)       AS total_players, "
            "  (SELECT COUNT(*) FROM play_events)   AS total_events");
        txn.commit();

        json j = {
            {"total_games",   r[0]["total_games"].as<int64_t>(0)},
            {"total_players", r[0]["total_players"].as<int64_t>(0)},
            {"total_events",  r[0]["total_events"].as<int64_t>(0)}
        };
        res.json(j.dump());
    } catch (const std::exception& e) {
        log->error("stats DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

// -- /api/leaderboard --------------------------------------------------------

void handle_leaderboard(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }

    std::string stat = req.query("stat", "ppg");

    // Whitelist of allowed sort columns
    std::string order_col = "ppg";
    if      (stat == "rpg")    order_col = "rpg";
    else if (stat == "spg")    order_col = "spg";
    else if (stat == "bpg")    order_col = "bpg";
    else if (stat == "fg_pct") order_col = "fg_pct";
    else if (stat == "ft_pct") order_col = "ft_pct";

    auto log = cortex::get_logger("http");
    try {
        pqxx::work txn(*ctx.db);
        auto r = txn.exec(
            "SELECT p.player_id, p.first_name || ' ' || p.last_name AS name, "
            "       t.tricode AS team, COALESCE(p.position, '') AS position, "
            "       SUM(pgs.points) AS pts, "
            "       SUM(pgs.rebounds) AS reb, "
            "       SUM(pgs.steals) AS stl, "
            "       SUM(pgs.blocks) AS blk, "
            "       COUNT(DISTINCT pgs.game_id) AS games, "
            "       ROUND(SUM(pgs.points)::numeric   / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS ppg, "
            "       ROUND(SUM(pgs.rebounds)::numeric  / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS rpg, "
            "       ROUND(SUM(pgs.steals)::numeric    / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS spg, "
            "       ROUND(SUM(pgs.blocks)::numeric    / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS bpg, "
            "       ROUND(SUM(pgs.fgm)::numeric * 100 / NULLIF(SUM(pgs.fga),0), 1) AS fg_pct, "
            "       ROUND(SUM(pgs.ftm)::numeric * 100 / NULLIF(SUM(pgs.fta),0), 1) AS ft_pct "
            "FROM player_game_stats pgs "
            "JOIN players p ON p.player_id = pgs.player_id "
            "JOIN teams t   ON t.team_id = p.team_id "
            "GROUP BY p.player_id, name, t.tricode, p.position "
            "HAVING COUNT(DISTINCT pgs.game_id) >= 30 "
            "ORDER BY " + order_col + " DESC NULLS LAST LIMIT 50");
        txn.commit();

        json players = json::array();
        for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
            players.push_back({
                {"rank",      static_cast<int>(i + 1)},
                {"player_id", r[i]["player_id"].as<int>()},
                {"name",      r[i]["name"].as<std::string>()},
                {"team",      r[i]["team"].as<std::string>()},
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

        json j = {{"stat", stat}, {"players", std::move(players)}};
        res.json(j.dump());
    } catch (const std::exception& e) {
        log->error("leaderboard DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

} // namespace cortex::serving::handlers
