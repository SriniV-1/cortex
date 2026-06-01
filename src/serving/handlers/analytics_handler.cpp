#include "serving/handlers/analytics_handler.hpp"
#include "serving/HttpUtils.hpp"
#include "analytics/GameStateIndex.hpp"
#include "analytics/EloTracker.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace cortex::serving::handlers {

// ── /api/elo ────────────────────────────────────────────────────────────────

void handle_elo_rankings(Request& /*req*/, Response& res, ServerContext& ctx) {
    if (!ctx.elo_tracker || !ctx.elo_tracker->built()) {
        res.json(R"({"error":"Elo ratings not ready yet — building in background"})", 503);
        return;
    }
    auto ratings = ctx.elo_tracker->all_ratings();
    std::ostringstream j;
    j << "{\"games_processed\":" << ctx.elo_tracker->games_processed()
      << ",\"build_ms\":" << std::fixed;
    j.precision(1);
    j << ctx.elo_tracker->build_ms()
      << ",\"teams\":[";
    for (size_t i = 0; i < ratings.size(); ++i) {
        if (i > 0) j << ",";
        const auto& te = ratings[i];
        j << "{\"rank\":" << (i + 1)
          << ",\"team_id\":" << te.team_id
          << ",\"tricode\":" << json_str(te.tricode)
          << ",\"rating\":" << std::fixed;
        j.precision(0);
        j << te.rating
          << ",\"games\":" << te.games_played
          << ",\"wins\":" << te.wins
          << ",\"losses\":" << te.losses
          << "}";
    }
    j << "]}";
    res.json(j.str());
}

// ── /api/elo/history ────────────────────────────────────────────────────────

void handle_elo_history(Request& /*req*/, Response& res, ServerContext& ctx) {
    if (!ctx.elo_tracker || !ctx.elo_tracker->built()) {
        res.json(R"({"error":"Elo ratings not ready yet"})", 503);
        return;
    }
    auto history = ctx.elo_tracker->elo_history();
    std::ostringstream j;
    j << "{\"snapshots\":[";
    for (size_t i = 0; i < history.size(); ++i) {
        if (i > 0) j << ",";
        const auto& s = history[i];
        j << "{\"season\":" << s.season
          << ",\"tricode\":" << json_str(s.tricode)
          << ",\"rating\":" << std::fixed;
        j.precision(0);
        j << s.rating << "}";
    }
    j << "]}";
    res.json(j.str());
}

// ── /api/index/status ───────────────────────────────────────────────────────

void handle_index_status(Request& /*req*/, Response& res, ServerContext& ctx) {
    std::ostringstream j;
    if (ctx.game_state_index && ctx.game_state_index->loaded()) {
        j << "{\"loaded\":true"
          << ",\"size\":"      << ctx.game_state_index->size()
          << ",\"build_ms\":"  << static_cast<long long>(ctx.game_state_index->build_ms())
          << "}";
    } else {
        j << "{\"loaded\":false,\"size\":0,\"build_ms\":0}";
    }
    res.json(j.str());
}

// ── /api/similar ────────────────────────────────────────────────────────────

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

    std::ostringstream j;
    j << "{"
      << "\"query\":{"
      << "\"score_home\":"  << score_home << ","
      << "\"score_away\":"  << score_away << ","
      << "\"period\":"      << period     << ","
      << "\"clock\":"       << clock      << ","
      << "\"momentum\":"    << momentum
      << "},"
      << "\"query_ms\":"    << std::fixed;
    j.precision(2);
    j << qms << ","
      << "\"index_size\":"  << ctx.game_state_index->size() << ","
      << "\"results\":[";

    for (size_t ri = 0; ri < matches.size(); ++ri) {
        const auto& m = matches[ri];
        if (ri > 0) j << ",";
        j << "{"
          << "\"rank\":"       << (ri + 1)          << ","
          << "\"event_id\":"   << m.event_id        << ","
          << "\"game_id\":"    << json_str(m.game_id) << ","
          << "\"home\":"       << json_str(m.home_tricode) << ","
          << "\"away\":"       << json_str(m.away_tricode) << ","
          << "\"date\":"       << json_str(m.date)   << ","
          << "\"score_home\":" << m.score_home       << ","
          << "\"score_away\":" << m.score_away       << ","
          << "\"period\":"     << static_cast<int>(m.period) << ","
          << "\"home_won\":"   << (m.home_won ? "true" : "false") << ","
          << "\"similarity\":" << std::fixed;
        j.precision(4);
        j << m.similarity
          << "}";
    }
    j << "]}";

    // Cache similar-moments results for 5 minutes
    if (ctx.cache) {
        const std::string ckey = "cortex:similar:" + std::to_string(score_home)
            + ":" + std::to_string(score_away) + ":" + std::to_string(period)
            + ":" + std::to_string(clock);
        ctx.cache->set(ckey, j.str(), std::chrono::seconds{300});
    }

    res.json(j.str());
}

} // namespace cortex::serving::handlers
