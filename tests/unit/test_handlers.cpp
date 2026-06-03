// Unit tests for Phase 6.3 — handler tests with mock ICache and IDatabase.
//
// These tests exercise handler functions directly without requiring a running
// server, Redis, or PostgreSQL — all dependencies are mocked via the DI interfaces.

#include "serving/ICache.hpp"
#include "serving/IDatabase.hpp"
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"
#include "serving/handlers/health_handler.hpp"
#include "serving/handlers/stats_handler.hpp"
#include "serving/handlers/metrics_handler.hpp"
#include "stream/StatAccumulator.hpp"
#include "stream/StreamEvent.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>

using json = nlohmann::json;
using namespace cortex::serving;
using namespace cortex::serving::handlers;
using namespace cortex::stream;

// ── Mock implementations ──────────────────────────────────────────────────

class MockCache : public ICache {
public:
    std::unordered_map<std::string, std::string> store;
    bool is_connected = true;

    std::optional<std::string> get(const std::string& key) override {
        auto it = store.find(key);
        if (it != store.end()) return it->second;
        return std::nullopt;
    }

    void set(const std::string& key, const std::string& value, int /*ttl_sec*/) override {
        store[key] = value;
    }

    void del(const std::string& key) override {
        store.erase(key);
    }

    bool connected() const override { return is_connected; }
};

class MockDatabase : public IDatabase {
public:
    std::vector<DbRow> result;
    bool is_connected = true;
    std::string last_sql;

    std::vector<DbRow> query(const std::string& sql) override {
        last_sql = sql;
        return result;
    }

    bool connected() const override { return is_connected; }
};

// ── Health handler ────────────────────────────────────────────────────────

TEST(HandlerTest, HealthReturnsOk) {
    Request req;
    Response res;
    ServerContext ctx;

    handle_health(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 200);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["status"], "ok");
    EXPECT_EQ(j["service"], "cortex");
}

// ── Game stats handler — cache miss path ──────────────────────────────────

TEST(HandlerTest, GameStatsCacheMissComputesAndCaches) {
    // Set up a StatAccumulator with a known game
    StatAccumulator acc;
    StreamEvent ev{};
    ev.player_id   = 1;
    ev.action_type = ActionType::Shot2pt;
    ev.shot_made   = true;
    ev.score_home  = 10;
    ev.score_away  = 5;
    const char* gid = "0022300042";
    std::copy(gid, gid + 11, ev.game_id.begin());
    acc.process(ev);

    MockCache cache;

    Request req;
    req.path_params["gameId"] = "0022300042";

    Response res;

    ServerContext ctx;
    ctx.accumulator = &acc;
    ctx.cache       = &cache;

    handle_game_stats(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 200);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["game_id"], "0022300042");
    EXPECT_EQ(j["score_home"], 10);
    EXPECT_EQ(j["score_away"], 5);

    // The handler should have cached the result
    auto cached = cache.get("cortex:stats:0022300042");
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(*cached, res.body);
}

// ── Game stats handler — cache hit path ───────────────────────────────────

TEST(HandlerTest, GameStatsCacheHitReturnsCachedValue) {
    StatAccumulator acc;
    MockCache cache;

    // Pre-populate cache with a known response
    std::string cached_body = R"({"game_id":"0022300042","cached":true})";
    cache.set("cortex:stats:0022300042", cached_body, 60);

    Request req;
    req.path_params["gameId"] = "0022300042";

    Response res;

    ServerContext ctx;
    ctx.accumulator = &acc;
    ctx.cache       = &cache;

    handle_game_stats(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body, cached_body);
}

// ── Game stats handler — missing game_id ──────────────────────────────────

TEST(HandlerTest, GameStatsMissingGameIdReturns400) {
    Request req;
    // No gameId path param set
    Response res;
    ServerContext ctx;

    handle_game_stats(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 400);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"], "missing game_id");
}

// ── Game stats handler — no accumulator ───────────────────────────────────

TEST(HandlerTest, GameStatsNoAccumulatorReturns500) {
    Request req;
    req.path_params["gameId"] = "0022300042";
    Response res;

    ServerContext ctx;
    ctx.accumulator = nullptr;

    handle_game_stats(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 500);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"], "no accumulator");
}

// ── Metrics handler — with accumulator ────────────────────────────────────

TEST(HandlerTest, MetricsIncludesEventCount) {
    StatAccumulator acc;
    // Process a couple of events so event_count > 0
    StreamEvent ev{};
    ev.player_id   = 7;
    ev.action_type = ActionType::Rebound;
    ev.shot_made   = false;
    ev.score_home  = 0;
    ev.score_away  = 0;
    const char* gid = "0022300099";
    std::copy(gid, gid + 11, ev.game_id.begin());
    acc.process(ev);
    acc.process(ev);

    Request req;
    Response res;
    ServerContext ctx;
    ctx.accumulator = &acc;

    handle_metrics(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 200);
    EXPECT_NE(res.content_type.find("text/plain"), std::string::npos);
    EXPECT_NE(res.body.find("cortex_events_processed"), std::string::npos);
    EXPECT_NE(res.body.find("cortex_active_games"), std::string::npos);
}

// ── Player season handler — no DB returns 500 ─────────────────────────────

TEST(HandlerTest, PlayerSeasonNoDbReturns500) {
    Request req;
    req.path_params["playerId"] = "23";
    Response res;
    ServerContext ctx;
    ctx.db = nullptr;

    handle_player_season(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 500);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"], "no db");
}

// ── Player season handler — invalid player_id returns 400 ─────────────────

TEST(HandlerTest, PlayerSeasonInvalidIdReturns400) {
    Request req;
    req.path_params["playerId"] = "not_a_number";
    Response res;
    ServerContext ctx;

    handle_player_season(req, res, ctx);

    EXPECT_TRUE(res.handled);
    EXPECT_EQ(res.status_code, 400);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"], "invalid player_id");
}
