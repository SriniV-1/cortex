#include "serving/handlers/games_handler.hpp"
#include "etl/LiveIngestor.hpp"
#include "common/Logger.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

using json = nlohmann::json;

namespace cortex::serving::handlers {

// -- /api/games/recent -------------------------------------------------------

void handle_games_recent(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.db) {
        res.json(R"({"error":"no db"})", 500);
        return;
    }

    std::string type_filter = req.query("type");
    auto log = cortex::get_logger("http");

    try {
        pqxx::work txn(*ctx.db);
        pqxx::result r;

        if (type_filter == "regular") {
            r = txn.exec(
                "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
                "       ht.tricode AS home, at.tricode AS away, "
                "       ht.full_name AS home_name, at.full_name AS away_name "
                "FROM games g "
                "JOIN teams ht ON ht.team_id = g.home_team_id "
                "JOIN teams at ON at.team_id = g.away_team_id "
                "WHERE g.season_type = 'Regular Season' "
                "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 20");
        } else if (type_filter == "playoffs") {
            r = txn.exec(
                "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
                "       ht.tricode AS home, at.tricode AS away, "
                "       ht.full_name AS home_name, at.full_name AS away_name "
                "FROM games g "
                "JOIN teams ht ON ht.team_id = g.home_team_id "
                "JOIN teams at ON at.team_id = g.away_team_id "
                "WHERE g.season_type = 'Playoffs' "
                "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 20");
        } else {
            r = txn.exec(
                "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
                "       ht.tricode AS home, at.tricode AS away, "
                "       ht.full_name AS home_name, at.full_name AS away_name "
                "FROM games g "
                "JOIN teams ht ON ht.team_id = g.home_team_id "
                "JOIN teams at ON at.team_id = g.away_team_id "
                "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 20");
        }
        txn.commit();

        json arr = json::array();
        for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
            arr.push_back({
                {"game_id",    r[i]["game_id"].as<std::string>()},
                {"date",       r[i]["game_date"].as<std::string>()},
                {"home",       r[i]["home"].as<std::string>()},
                {"away",       r[i]["away"].as<std::string>()},
                {"home_name",  r[i]["home_name"].as<std::string>()},
                {"away_name",  r[i]["away_name"].as<std::string>()},
                {"home_score", r[i]["home_score"].as<int>(0)},
                {"away_score", r[i]["away_score"].as<int>(0)},
                {"status",     r[i]["status"].as<int>(1)}
            });
        }
        res.json(arr.dump());
    } catch (const std::exception& e) {
        log->error("games/recent DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

// -- /api/scoreboard ---------------------------------------------------------

void handle_scoreboard(Request& /*req*/, Response& res, ServerContext& ctx) {
    if (!ctx.live_ingestor) {
        res.json(R"json({"error":"live ingestion not enabled -- start with --live"})json", 503);
        return;
    }
    auto games = ctx.live_ingestor->scoreboard_snapshot();

    json games_arr = json::array();
    for (const auto& g : games) {
        games_arr.push_back({
            {"game_id",    g.game_id},
            {"status",     g.status},
            {"home",       g.home_tricode},
            {"away",       g.away_tricode},
            {"home_score", g.home_score},
            {"away_score", g.away_score},
            {"period",     g.period},
            {"game_clock", g.game_clock}
        });
    }

    json j = {{"games", std::move(games_arr)}};
    res.json(j.dump());
}

} // namespace cortex::serving::handlers
