#include <gtest/gtest.h>
#include "etl/NBAClient.hpp"

namespace cortex::etl {

// ── game_ids_for_season ────────────────────────────────────────────────────

TEST(NBAClientTest, GameIdFormat_2023Season) {
    auto ids = NBAClient::game_ids_for_season(2023, 5);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], "0022300001");
    EXPECT_EQ(ids[1], "0022300002");
    EXPECT_EQ(ids[4], "0022300005");
}

TEST(NBAClientTest, GameIdFormat_2020Season) {
    auto ids = NBAClient::game_ids_for_season(2020, 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], "0022000001");
}

TEST(NBAClientTest, GameIdFormat_1999Season) {
    auto ids = NBAClient::game_ids_for_season(1999, 1);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "0029900001");
}

TEST(NBAClientTest, GameIdFormat_CorrectCount) {
    auto ids = NBAClient::game_ids_for_season(2023, 1230);
    EXPECT_EQ(ids.size(), 1230u);
    EXPECT_EQ(ids.back(), "0022301230");
}

// ── Network tests (require S3 access) — skip in CI if no network ──────────
// These are tagged LIVE and only run if CORTEX_RUN_LIVE_TESTS=1

class NBAClientLiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CORTEX_RUN_LIVE_TESTS")) {
            GTEST_SKIP() << "Set CORTEX_RUN_LIVE_TESTS=1 to run live tests";
        }
    }
};

TEST_F(NBAClientLiveTest, FetchScoreboard) {
    NBAClient client;
    auto games = client.fetch_scoreboard();
    // Scoreboard always returns >= 0 games (may be 0 on off-season dates)
    EXPECT_GE(games.size(), 0u);
    for (const auto& g : games) {
        EXPECT_EQ(g.game_id.size(), 10u);
        EXPECT_FALSE(g.away_tricode.empty());
        EXPECT_FALSE(g.home_tricode.empty());
    }
}

TEST_F(NBAClientLiveTest, FetchKnownGame) {
    NBAClient client;
    // Game 0022300001 (2023-24 season opener) is permanently available on S3
    auto pbp = client.fetch_play_by_play("0022300001");
    ASSERT_TRUE(pbp.has_value());
    EXPECT_EQ(pbp->game_id, "0022300001");
    EXPECT_GT(pbp->actions.size(), 100u);  // games have 400-600 actions

    // Verify first action has required fields
    const auto& first = pbp->actions.front();
    EXPECT_GT(first.action_number, 0);
    EXPECT_GE(first.period, 1);
    EXPECT_FALSE(first.action_type.empty());
}

TEST_F(NBAClientLiveTest, FetchNonExistentGame) {
    NBAClient client;
    // All-zeros game ID should return nullopt (404)
    auto pbp = client.fetch_play_by_play("0022399999");
    EXPECT_FALSE(pbp.has_value());
}

} // namespace cortex::etl
