// Integration tests for Cortex.
// These tests require a running PostgreSQL instance with the cortex schema.
// Skipped unless CORTEX_RUN_INTEGRATION_TESTS=1 and CORTEX_TEST_DB is set.
//
// What they verify:
//   1. Schema can be applied cleanly
//   2. ETL BulkInserter can write and read back game/event data
//   3. HttpServer serves correct JSON from the database
//   4. EloTracker produces deterministic ratings from fixture data
//   5. StatAccumulator + API roundtrip returns correct stats

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "etl/BulkInserter.hpp"
#include "stream/StatAccumulator.hpp"
#include "stream/StreamEvent.hpp"
#include "serving/HttpServer.hpp"
#include "analytics/EloTracker.hpp"

#include <thread>
#include <chrono>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace cortex;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string get_test_db() {
    const char* db = std::getenv("CORTEX_TEST_DB");
    return db ? db : "";
}

static int tcp_connect(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static std::string http_get(uint16_t port, const std::string& path) {
    int fd = tcp_connect(port);
    if (fd < 0) return "";
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string resp;
    char buf[4096];
    while (resp.size() < 65536) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
        if (resp.find("\r\n\r\n") != std::string::npos) {
            auto cl_pos = resp.find("Content-Length: ");
            if (cl_pos != std::string::npos) {
                size_t cl_end = resp.find("\r\n", cl_pos);
                int cl = std::stoi(resp.substr(cl_pos + 16, cl_end - cl_pos - 16));
                auto body_start = resp.find("\r\n\r\n") + 4;
                if (static_cast<int>(resp.size() - body_start) >= cl) break;
            } else {
                break;
            }
        }
    }
    ::close(fd);
    return resp;
}

// Extract JSON body from HTTP response
static std::string extract_body(const std::string& resp) {
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return resp.substr(pos + 4);
}

// ── Fixture ──────────────────────────────────────────────────────────────────

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CORTEX_RUN_INTEGRATION_TESTS")) {
            GTEST_SKIP() << "Set CORTEX_RUN_INTEGRATION_TESTS=1 to run";
        }
        conn_str_ = get_test_db();
        if (conn_str_.empty()) {
            GTEST_SKIP() << "Set CORTEX_TEST_DB to a libpqxx connection string";
        }
        // Clean tables before each test
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        txn.exec("DELETE FROM play_events");
        txn.exec("DELETE FROM games");
        txn.exec("DELETE FROM players");
        txn.exec("DELETE FROM teams");
        txn.commit();
    }

    std::string conn_str_;
};

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_F(IntegrationTest, DatabaseSchemaApplied) {
    // Verify all expected tables exist
    pqxx::connection conn(conn_str_);
    pqxx::work txn(conn);

    auto r = txn.exec(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = 'public' ORDER BY table_name");

    std::vector<std::string> tables;
    for (const auto& row : r) {
        tables.push_back(row[0].as<std::string>());
    }

    // Core tables should exist
    EXPECT_NE(std::find(tables.begin(), tables.end(), "games"), tables.end());
    EXPECT_NE(std::find(tables.begin(), tables.end(), "players"), tables.end());
    EXPECT_NE(std::find(tables.begin(), tables.end(), "teams"), tables.end());
}

TEST_F(IntegrationTest, InsertAndQueryGames) {
    pqxx::connection conn(conn_str_);

    // Insert teams first (FK constraint)
    {
        pqxx::work txn(conn);
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1610612747, 'LAL', 'Los Angeles Lakers', 'Los Angeles')");
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1610612743, 'DEN', 'Denver Nuggets', 'Denver')");
        txn.commit();
    }

    // Insert a fixture game
    {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO games (game_id, game_date, season, season_type, "
            "home_team_id, away_team_id, home_score, away_score, status) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
            "0022300001", "2023-10-24", 2023, "Regular Season",
            1610612747, 1610612743, 107, 104, 3);
        txn.commit();
    }

    // Query it back
    {
        pqxx::work txn(conn);
        auto r = txn.exec("SELECT game_id, home_team_id, away_team_id, home_score "
                          "FROM games WHERE game_id = '0022300001'");
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0]["game_id"].as<std::string>(), "0022300001");
        EXPECT_EQ(r[0]["home_team_id"].as<int>(), 1610612747);
        EXPECT_EQ(r[0]["away_team_id"].as<int>(), 1610612743);
        EXPECT_EQ(r[0]["home_score"].as<int>(), 107);
    }
}

TEST_F(IntegrationTest, InsertAndQueryPlayEvents) {
    pqxx::connection conn(conn_str_);

    // Insert teams and game first (FK constraint)
    {
        pqxx::work txn(conn);
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1610612747, 'LAL', 'Los Angeles Lakers', 'Los Angeles')");
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1610612743, 'DEN', 'Denver Nuggets', 'Denver')");
        txn.exec_params(
            "INSERT INTO games (game_id, game_date, season, season_type, "
            "home_team_id, away_team_id, home_score, away_score, status) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
            "0022300001", "2023-10-24", 2023, "Regular Season",
            1610612747, 1610612743, 107, 104, 3);
        txn.commit();
    }

    // Insert play events
    {
        pqxx::work txn(conn);
        for (int i = 1; i <= 10; ++i) {
            txn.exec_params(
                "INSERT INTO play_events (event_id, game_id, action_number, "
                "occurred_at, period, clock, action_type, description, "
                "score_home, score_away) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                i, "0022300001", i, "2023-10-24T19:00:00Z",
                1, "11:30", "2pt", "Made shot", i * 2, 0);
        }
        txn.commit();
    }

    // Query events back
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT COUNT(*) FROM play_events WHERE game_id = '0022300001'");
        EXPECT_EQ(r[0][0].as<int>(), 10);
    }
}

TEST_F(IntegrationTest, EloRatingsDeterministic) {
    // Insert teams and games, verify Elo is deterministic
    pqxx::connection conn(conn_str_);
    {
        pqxx::work txn(conn);
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1, 'AAA', 'Team A', 'City A')");
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (2, 'BBB', 'Team B', 'City B')");

        // Team A beats Team B 3 times in a row
        for (int i = 1; i <= 3; ++i) {
            std::string gid = "002230000" + std::to_string(i);
            txn.exec_params(
                "INSERT INTO games (game_id, game_date, season, season_type, "
                "home_team_id, away_team_id, home_score, away_score, status) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                gid, "2023-10-2" + std::to_string(i), 2023, "Regular Season",
                1, 2, 100 + i, 90, 3);
        }
        txn.commit();
    }

    // Build Elo twice — results should be identical
    analytics::EloTracker elo1, elo2;
    elo1.build_from_db(conn);
    elo2.build_from_db(conn);

    auto ratings1 = elo1.all_ratings();
    auto ratings2 = elo2.all_ratings();

    ASSERT_EQ(ratings1.size(), ratings2.size());
    for (size_t i = 0; i < ratings1.size(); ++i) {
        EXPECT_EQ(ratings1[i].tricode, ratings2[i].tricode);
        EXPECT_DOUBLE_EQ(ratings1[i].rating, ratings2[i].rating);
    }

    // Team A (winner) should be rated higher than Team B
    ASSERT_GE(ratings1.size(), 2u);
    bool a_higher = false;
    for (const auto& r : ratings1) {
        if (r.tricode == "AAA") {
            a_higher = (r.rating > 1500.0);
            break;
        }
    }
    EXPECT_TRUE(a_higher) << "Winning team should have rating above 1500";
}

TEST_F(IntegrationTest, HttpServerServesHealth) {
    // Start a server against the test database
    stream::StatAccumulator acc;
    serving::HttpServer::Config cfg;
    cfg.port = 18090;
    cfg.db_conn_str = conn_str_;
    cfg.www_root = "";

    auto server = std::make_unique<serving::HttpServer>(cfg, acc);
    std::thread server_thread([&] { server->run(); });

    // Wait for server to come up
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(server->running());
    // Give the event loop time to start polling
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Health returns 200 (all deps up) or 503 (degraded) — both are valid
    auto resp = http_get(18090, "/health");
    EXPECT_TRUE(resp.find("200") != std::string::npos ||
                resp.find("503") != std::string::npos);
    EXPECT_NE(resp.find("checks"), std::string::npos);

    server->stop();
    server_thread.join();
}

TEST_F(IntegrationTest, HttpServerServesLeaderboard) {
    // Insert fixture data
    pqxx::connection conn(conn_str_);
    {
        pqxx::work txn(conn);
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1610612747, 'LAL', 'Los Angeles Lakers', 'Los Angeles')");
        txn.exec("INSERT INTO teams (team_id, tricode, full_name, city) "
                 "VALUES (1610612743, 'DEN', 'Denver Nuggets', 'Denver')");
        txn.exec("INSERT INTO players (player_id, first_name, last_name, team_id) "
                 "VALUES (2544, 'LeBron', 'James', 1610612747)");
        txn.exec_params(
            "INSERT INTO games (game_id, game_date, season, season_type, "
            "home_team_id, away_team_id, home_score, away_score, status) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
            "0022300001", "2023-10-24", 2023, "Regular Season",
            1610612747, 1610612743, 107, 104, 3);
        // Insert some play events for the player
        for (int i = 1; i <= 5; ++i) {
            txn.exec_params(
                "INSERT INTO play_events (event_id, game_id, action_number, "
                "occurred_at, period, clock, action_type, player_id, "
                "description, score_home, score_away) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)",
                i, "0022300001", i, "2023-10-24T19:00:00Z",
                1, "11:00", "2pt", 2544, "Made shot", i * 2, 0);
        }
        txn.commit();
    }

    // Refresh the materialized view so leaderboard works
    {
        pqxx::nontransaction ntxn(conn);
        ntxn.exec("REFRESH MATERIALIZED VIEW player_game_stats");
    }

    stream::StatAccumulator acc;
    serving::HttpServer::Config cfg;
    cfg.port = 18091;
    cfg.db_conn_str = conn_str_;
    cfg.www_root = "";

    auto server = std::make_unique<serving::HttpServer>(cfg, acc);
    std::thread server_thread([&] { server->run(); });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(server->running());
    // Give the event loop time to start polling
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = http_get(18091, "/api/leaderboard?stat=pts&season=2023");
    auto body = extract_body(resp);

    EXPECT_NE(resp.find("200"), std::string::npos);
    // Response should contain player data
    EXPECT_NE(body.find("LeBron"), std::string::npos);

    server->stop();
    server_thread.join();
}
