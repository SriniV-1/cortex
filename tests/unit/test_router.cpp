// Unit tests for the Router class — trie-based HTTP route dispatcher.

#include "serving/Router.hpp"
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

#include <gtest/gtest.h>

using namespace cortex::serving;

class RouterTest : public ::testing::Test {
protected:
    Router router;

    // Dummy handler that records which handler was called.
    static void make_handler(const std::string& tag,
                              Request& req, Response& res, ServerContext&) {
        res.json("{\"handler\":\"" + tag + "\"}");
    }
};

TEST_F(RouterTest, ExactPathMatch) {
    router.add("GET", "/health",
               [](Request& req, Response& res, ServerContext& ctx) {
                   make_handler("health", req, res, ctx);
               });

    auto result = router.match("GET", "/health");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->params.empty());

    Request req; Response res; ServerContext ctx;
    result->handler(req, res, ctx);
    EXPECT_TRUE(res.handled);
    EXPECT_NE(res.body.find("health"), std::string::npos);
}

TEST_F(RouterTest, PathParameterExtraction) {
    router.add("GET", "/stats/:gameId",
               [](Request& req, Response& res, ServerContext& ctx) {
                   make_handler("stats", req, res, ctx);
               });

    auto result = router.match("GET", "/stats/0022300123");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->params.at("gameId"), "0022300123");
}

TEST_F(RouterTest, MultiplePathParameters) {
    router.add("GET", "/players/:playerId/season",
               [](Request& req, Response& res, ServerContext& ctx) {
                   make_handler("player_season", req, res, ctx);
               });

    auto result = router.match("GET", "/players/203999/season");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->params.at("playerId"), "203999");
}

TEST_F(RouterTest, NoMatchReturnsNullopt) {
    router.add("GET", "/health",
               [](Request&, Response& res, ServerContext&) { res.json("{}"); });

    auto result = router.match("GET", "/nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST_F(RouterTest, MethodFiltering) {
    router.add("GET", "/health",
               [](Request&, Response& res, ServerContext&) {
                   res.json("{\"method\":\"GET\"}");
               });
    router.add("POST", "/health",
               [](Request&, Response& res, ServerContext&) {
                   res.json("{\"method\":\"POST\"}");
               });

    auto get_result = router.match("GET", "/health");
    ASSERT_TRUE(get_result.has_value());

    auto post_result = router.match("POST", "/health");
    ASSERT_TRUE(post_result.has_value());

    // DELETE should not match
    auto del_result = router.match("DELETE", "/health");
    EXPECT_FALSE(del_result.has_value());
}

TEST_F(RouterTest, WrongMethodReturnsNullopt) {
    router.add("POST", "/data",
               [](Request&, Response& res, ServerContext&) { res.json("{}"); });

    auto result = router.match("GET", "/data");
    EXPECT_FALSE(result.has_value());
}

TEST_F(RouterTest, LiteralTakesPriorityOverParam) {
    // /api/stats is a literal route
    router.add("GET", "/api/stats",
               [](Request& req, Response& res, ServerContext& ctx) {
                   make_handler("api_stats", req, res, ctx);
               });
    // /stats/:gameId is a param route — different prefix
    router.add("GET", "/stats/:gameId",
               [](Request& req, Response& res, ServerContext& ctx) {
                   make_handler("game_stats", req, res, ctx);
               });

    auto api_result = router.match("GET", "/api/stats");
    ASSERT_TRUE(api_result.has_value());
    EXPECT_TRUE(api_result->params.empty());

    auto game_result = router.match("GET", "/stats/001");
    ASSERT_TRUE(game_result.has_value());
    EXPECT_EQ(game_result->params.at("gameId"), "001");
}

TEST_F(RouterTest, NestedApiRoutes) {
    router.add("GET", "/api/elo",
               [](Request&, Response& res, ServerContext&) { res.json("{\"route\":\"elo\"}"); });
    router.add("GET", "/api/elo/history",
               [](Request&, Response& res, ServerContext&) { res.json("{\"route\":\"elo_history\"}"); });

    auto elo = router.match("GET", "/api/elo");
    ASSERT_TRUE(elo.has_value());

    auto hist = router.match("GET", "/api/elo/history");
    ASSERT_TRUE(hist.has_value());

    // Ensure they are distinct
    Request req; Response res1, res2; ServerContext ctx;
    elo->handler(req, res1, ctx);
    hist->handler(req, res2, ctx);
    EXPECT_NE(res1.body, res2.body);
}

TEST_F(RouterTest, EmptyRouterReturnsNullopt) {
    Router empty;
    auto result = empty.match("GET", "/anything");
    EXPECT_FALSE(result.has_value());
}

TEST_F(RouterTest, RootPath) {
    router.add("GET", "/",
               [](Request&, Response& res, ServerContext&) { res.json("{\"root\":true}"); });

    // The root path "/" splits into zero segments.
    // The handler should be on the root node.
    auto result = router.match("GET", "/");
    ASSERT_TRUE(result.has_value());
}

TEST_F(RouterTest, WebSocketRouteExtractsGameId) {
    router.add("GET", "/live/:gameId",
               [](Request&, Response& res, ServerContext&) { res.json("{}"); });

    auto result = router.match("GET", "/live/0022300001");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->params.at("gameId"), "0022300001");
}
