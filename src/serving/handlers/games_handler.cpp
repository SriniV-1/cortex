#include "serving/handlers/games_handler.hpp"
#include "serving/Pagination.hpp"
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
    auto page = parse_pagination(req);
    auto log = cortex::get_logger("http");

    // Decode cursor — expects {"game_id": "00223..."}
    std::string cursor_game_id;
    if (!page.cursor.empty()) {
        auto cur = decode_cursor(page.cursor);
        if (!cur.is_null() && cur.contains("game_id")) {
            cursor_game_id = cur["game_id"].get<std::string>();
        }
    }

    int fetch_limit = page.limit + 1;  // fetch one extra to detect has_more

    try {
        pqxx::work txn(*ctx.db);
        pqxx::result r;

        std::string base_sql =
            "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
            "       ht.tricode AS home, at.tricode AS away, "
            "       ht.full_name AS home_name, at.full_name AS away_name "
            "FROM games g "
            "JOIN teams ht ON ht.team_id = g.home_team_id "
            "JOIN teams at ON at.team_id = g.away_team_id ";

        std::string where_clause;
        if (type_filter == "regular") {
            where_clause = "WHERE g.season_type = 'Regular Season' ";
        } else if (type_filter == "playoffs") {
            where_clause = "WHERE g.season_type = 'Playoffs' ";
        }

        std::string order_clause = "ORDER BY g.game_date DESC, g.game_id DESC ";

        if (!cursor_game_id.empty()) {
            std::string cursor_cond = (where_clause.empty() ? "WHERE " : "AND ") +
                std::string("g.game_id < $1 ");
            r = txn.exec_params(
                base_sql + where_clause + cursor_cond + order_clause + "LIMIT $2",
                cursor_game_id.c_str(), fetch_limit);
        } else {
            r = txn.exec_params(
                base_sql + where_clause + order_clause + "LIMIT $1",
                fetch_limit);
        }
        txn.commit();

        bool has_more = static_cast<int>(r.size()) > page.limit;
        int return_count = has_more ? page.limit : static_cast<int>(r.size());

        json arr = json::array();
        for (int i = 0; i < return_count; ++i) {
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

        std::string next_cursor;
        if (has_more && return_count > 0) {
            next_cursor = encode_cursor(
                json{{"game_id", r[return_count - 1]["game_id"].as<std::string>()}});
        }

        res.json(paginated_response(arr, next_cursor, has_more).dump());
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
