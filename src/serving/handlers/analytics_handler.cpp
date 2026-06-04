#include "serving/handlers/analytics_handler.hpp"
#include "serving/Pagination.hpp"
#include "analytics/GameStateIndex.hpp"
#include "analytics/EloTracker.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>

using json = nlohmann::json;

namespace cortex::serving::handlers {

// -- /api/elo ----------------------------------------------------------------

void handle_elo_rankings(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.elo_tracker || !ctx.elo_tracker->built()) {
        res.json(R"({"error":"Elo ratings not ready yet — building in background"})", 503);
        return;
    }
    auto ratings = ctx.elo_tracker->all_ratings();
    auto page = parse_pagination(req);

    // Decode cursor — expects {"rank": N} (1-based index into sorted vector)
    size_t start_idx = 0;
    if (!page.cursor.empty()) {
        auto cur = decode_cursor(page.cursor);
        if (!cur.is_null() && cur.contains("rank")) {
            start_idx = static_cast<size_t>(cur["rank"].get<int>());
        }
    }

    if (start_idx > ratings.size()) start_idx = ratings.size();

    size_t end_idx = std::min(start_idx + static_cast<size_t>(page.limit), ratings.size());
    bool has_more = end_idx < ratings.size();

    json teams_arr = json::array();
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& te = ratings[i];
        teams_arr.push_back({
            {"rank",     static_cast<int>(i + 1)},
            {"team_id",  te.team_id},
            {"tricode",  te.tricode},
            {"rating",   static_cast<int>(std::round(te.rating))},
            {"games",    te.games_played},
            {"wins",     te.wins},
            {"losses",   te.losses}
        });
    }

    std::string next_cursor;
    if (has_more) {
        next_cursor = encode_cursor(json{{"rank", static_cast<int>(end_idx)}});
    }

    json j = paginated_response(teams_arr, next_cursor, has_more);
    j["games_processed"] = ctx.elo_tracker->games_processed();
    j["build_ms"]        = std::round(ctx.elo_tracker->build_ms() * 10.0) / 10.0;
    res.json(j.dump());
}

// -- /api/elo/history --------------------------------------------------------

void handle_elo_history(Request& /*req*/, Response& res, ServerContext& ctx) {
    if (!ctx.elo_tracker || !ctx.elo_tracker->built()) {
        res.json(R"({"error":"Elo ratings not ready yet"})", 503);
        return;
    }
    auto history = ctx.elo_tracker->elo_history();

    json snapshots = json::array();
    for (const auto& s : history) {
        snapshots.push_back({
            {"season",  s.season},
            {"tricode", s.tricode},
            {"rating",  static_cast<int>(std::round(s.rating))}
        });
    }

    json j = {{"snapshots", std::move(snapshots)}};
    res.json(j.dump());
}

// -- /api/index/status -------------------------------------------------------

void handle_index_status(Request& /*req*/, Response& res, ServerContext& ctx) {
    json j;
    if (ctx.game_state_index && ctx.game_state_index->loaded()) {
        j = {
            {"loaded",   true},
            {"size",     ctx.game_state_index->size()},
            {"build_ms", static_cast<long long>(ctx.game_state_index->build_ms())}
        };
    } else {
        j = {{"loaded", false}, {"size", 0}, {"build_ms", 0}};
    }
    res.json(j.dump());
}

// -- /api/similar ------------------------------------------------------------

void handle_similarity(Request& req, Response& res, ServerContext& ctx) {
    if (!ctx.game_state_index || !ctx.game_state_index->loaded()) {
        res.json(R"({"error":"similarity index not ready yet — building in background"})", 503);
        return;
    }

    const int score_home = req.query_int("score_home", 50);
    const int score_away = req.query_int("score_away", 50);
    const int period     = req.query_int("period",     2);
    const int clock      = req.query_int("clock",      360);
    const int momentum   = req.query_int("momentum",   0);
    const int k          = std::max(1, std::min(req.query_int("k", 10), 25));

    const auto t0 = std::chrono::steady_clock::now();

    auto qvec    = cortex::analytics::encode_game_state(
                       score_home, score_away, period, clock, momentum);
    auto matches = ctx.game_state_index->query(qvec, k);

    const auto t1      = std::chrono::steady_clock::now();
    const double qms   = std::chrono::duration<double, std::milli>(t1 - t0).count();

    json results = json::array();
    for (size_t ri = 0; ri < matches.size(); ++ri) {
        const auto& m = matches[ri];
        results.push_back({
            {"rank",       static_cast<int>(ri + 1)},
            {"event_id",   m.event_id},
            {"game_id",    m.game_id},
            {"home",       m.home_tricode},
            {"away",       m.away_tricode},
            {"date",       m.date},
            {"score_home", m.score_home},
            {"score_away", m.score_away},
            {"period",     static_cast<int>(m.period)},
            {"home_won",   m.home_won},
            {"similarity", std::round(m.similarity * 10000.0) / 10000.0}
        });
    }

    json j = {
        {"query", {
            {"score_home", score_home},
            {"score_away", score_away},
            {"period",     period},
            {"clock",      clock},
            {"momentum",   momentum}
        }},
        {"query_ms",   std::round(qms * 100.0) / 100.0},
        {"index_size", ctx.game_state_index->size()},
        {"results",    std::move(results)}
    };

    std::string body = j.dump();

    // Cache similar-moments results for 5 minutes
    if (ctx.cache) {
        const std::string ckey = "cortex:similar:" + std::to_string(score_home)
            + ":" + std::to_string(score_away) + ":" + std::to_string(period)
            + ":" + std::to_string(clock);
        ctx.cache->set(ckey, body, 300);
    }

    res.json(body);
}

} // namespace cortex::serving::handlers
