#include <gtest/gtest.h>
#include "etl/BulkInserter.hpp"
#include "etl/NBAClient.hpp"

namespace cortex::etl {

static constexpr const char* TEST_CONN =
    "host=localhost port=5433 dbname=cortex";

// ── Integration tests — require a running cortex DB ─────────────────────
class BulkInserterTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CORTEX_RUN_DB_TESTS")) {
            GTEST_SKIP() << "Set CORTEX_RUN_DB_TESTS=1 to run DB integration tests";
        }
    }

    // Build a minimal PlayByPlay with n synthetic actions.
    static PlayByPlay make_pbp(const std::string& game_id, int n) {
        PlayByPlay pbp;
        pbp.game_id = game_id;
        for (int i = 1; i <= n; ++i) {
            PlayAction a{};
            a.action_number  = i;
            a.clock          = "PT10M00.00S";
            a.time_actual    = "2024-01-01T20:00:00.0Z";
            a.period         = 1;
            a.period_type    = "REGULAR";
            a.action_type    = "shot";
            a.sub_type       = "2pt";
            a.description    = "Test shot";
            a.person_id      = 1629029;
            a.score_home     = 0;
            a.score_away     = 0;
            a.order_number   = i * 10000LL;
            a.qualifiers_json = "[]";
            pbp.actions.push_back(a);
        }
        return pbp;
    }
};

TEST_F(BulkInserterTest, InsertAndSkipDuplicate) {
    BulkInserter inserter(TEST_CONN);

    const std::string game_id = "0020099001";  // synthetic test game

    // Ensure team exists to satisfy FK
    cortex::etl::TeamInfo t;
    t.team_id = 1610612738;
    t.tricode = "BOS";
    t.name    = "Celtics";
    t.city    = "Boston";
    inserter.ensure_team(t);

    auto pbp = make_pbp(game_id, 50);
    int64_t n1 = inserter.bulk_insert_play_by_play(pbp, 2009);
    EXPECT_EQ(n1, 50);

    // Second call should skip (resume-safe)
    int64_t n2 = inserter.bulk_insert_play_by_play(pbp, 2009);
    EXPECT_EQ(n2, 0);
    EXPECT_EQ(inserter.stats().games_skipped, 1);
}

TEST_F(BulkInserterTest, IsGameLoaded) {
    BulkInserter inserter(TEST_CONN);
    EXPECT_FALSE(inserter.is_game_loaded("0020099999"));  // synthetic, never loaded
}

} // namespace cortex::etl
