#include "serving/handlers/games_handler.hpp"
#include "serving/HttpUtils.hpp"
#include "etl/LiveIngestor.hpp"
#include "common/Logger.hpp"

#include <pqxx/pqxx>
#include <sstream>

namespace cortex::serving::handlers {

// ── /api/games/recent ───────────────────────────────────────────────────────

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

        std::ostringstream j;
        j << "[";
        for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
            if (i > 0) j << ",";
            j << "{"
              << "\"game_id\":"    << json_str(r[i]["game_id"].as<std::string>()) << ","
              << "\"date\":"       << json_str(r[i]["game_date"].as<std::string>()) << ","
              << "\"home\":"       << json_str(r[i]["home"].as<std::string>()) << ","
              << "\"away\":"       << json_str(r[i]["away"].as<std::string>()) << ","
              << "\"home_name\":"  << json_str(r[i]["home_name"].as<std::string>()) << ","
              << "\"away_name\":"  << json_str(r[i]["away_name"].as<std::string>()) << ","
              << "\"home_score\":" << r[i]["home_score"].as<int>(0) << ","
              << "\"away_score\":" << r[i]["away_score"].as<int>(0) << ","
              << "\"status\":"     << r[i]["status"].as<int>(1)
              << "}";
        }
        j << "]";
        res.json(j.str());
    } catch (const std::exception& e) {
        log->error("games/recent DB error: {}", e.what());
        res.json(R"({"error":"db error"})", 500);
    }
}

// ── /api/scoreboard ─────────────────────────────────────────────────────────

void handle_scoreboard(Request& /*req*/, Response& res, ServerContext& ctx) {
    if (!ctx.live_ingestor) {
        res.json(R"json({"error":"live ingestion not enabled -- start with --live"})json", 503);
        return;
    }
    auto games = ctx.live_ingestor->scoreboard_snapshot();
    std::ostringstream j;
    j << "{\"games\":[";
    for (size_t i = 0; i < games.size(); ++i) {
        if (i > 0) j << ",";
        const auto& g = games[i];
        j << "{"
          << "\"game_id\":"   << json_str(g.game_id) << ","
          << "\"status\":"    << g.status << ","
          << "\"home\":"      << json_str(g.home_tricode) << ","
          << "\"away\":"      << json_str(g.away_tricode) << ","
          << "\"home_score\":" << g.home_score << ","
          << "\"away_score\":" << g.away_score << ","
          << "\"period\":"    << g.period << ","
          << "\"game_clock\":" << json_str(g.game_clock)
          << "}";
    }
    j << "]}";
    res.json(j.str());
}

} // namespace cortex::serving::handlers
