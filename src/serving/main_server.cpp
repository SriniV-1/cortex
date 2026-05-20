// Cortex analytics server — Phase 3 entry point.
//
// Wires together:
//   StreamProcessor  → consumes live events from RingBuffer
//   HttpServer       → serves REST + WebSocket clients
//   RedisCache       → TTL caching for aggregation queries

#include "serving/HttpServer.hpp"
#include "serving/RedisCache.hpp"
#include "stream/RingBuffer.hpp"
#include "stream/StreamEvent.hpp"
#include "stream/StreamProcessor.hpp"
#include "stream/StatAccumulator.hpp"
#include "analytics/WinProbModel.hpp"
#include "analytics/GameStateIndex.hpp"
#include "analytics/EloTracker.hpp"
#include "etl/LiveIngestor.hpp"
#include "etl/NBAClient.hpp"
#include "etl/BulkInserter.hpp"
#include "common/Logger.hpp"

#include <pqxx/pqxx>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>

using namespace cortex;
using namespace cortex::stream;
using namespace cortex::serving;
using namespace cortex::analytics;
using namespace cortex::etl;

// Graceful shutdown
static std::atomic<bool> g_shutdown{false};
static HttpServer*        g_server  = nullptr;

static void handle_signal(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_release);
    if (g_server) g_server->stop();
}

int main(int argc, char** argv) {
    // ── Config from env / args ────────────────────────────────────────────
    uint16_t    port             = 8080;
    std::string db_conn          = "host=localhost port=5433 dbname=cortex";
    std::string redis_host       = "127.0.0.1";
    int         redis_port       = 6379;
    bool        enable_live      = false;
    int         poll_interval_ms = 30'000;
    std::string www_root         = "www";

    std::string model_path = "data/models/win_prob.onnx";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc)
            db_conn = argv[++i];
        if (std::strcmp(argv[i], "--redis") == 0 && i + 1 < argc)
            redis_host = argv[++i];
        if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model_path = argv[++i];
        if (std::strcmp(argv[i], "--live") == 0)
            enable_live = true;
        if (std::strcmp(argv[i], "--poll-interval") == 0 && i + 1 < argc)
            poll_interval_ms = std::atoi(argv[++i]);
        if (std::strcmp(argv[i], "--www") == 0 && i + 1 < argc)
            www_root = argv[++i];
    }

    auto log = cortex::get_logger("main");
    log->info("Cortex server starting on port {}", port);

    // ── Infrastructure ────────────────────────────────────────────────────
    RingBuffer<StreamEvent> ring(65536);
    StatAccumulator         accumulator;
    RedisCache              cache(redis_host, redis_port);

    // ── Similarity index (built in background — HTTP server starts immediately) ──
    GameStateIndex sim_index;
    std::jthread   sim_builder;
    if (!db_conn.empty()) {
        sim_builder = std::jthread([&]() {
            try {
                pqxx::connection build_conn(db_conn);
                sim_index.build_from_db(build_conn);
            } catch (const std::exception& e) {
                auto lg = cortex::get_logger("similarity");
                lg->warn("Similarity index build skipped: {}", e.what());
            }
        });
    }

    // ── Elo ratings (built in background alongside similarity index) ──────
    EloTracker elo_tracker;
    std::jthread elo_builder;
    if (!db_conn.empty()) {
        elo_builder = std::jthread([&]() {
            try {
                pqxx::connection elo_conn(db_conn);
                elo_tracker.build_from_db(elo_conn);
                elo_tracker.save_to_db(elo_conn);
            } catch (const std::exception& e) {
                auto lg = cortex::get_logger("elo");
                lg->warn("Elo build skipped: {}", e.what());
            }
        });
    }

    // ── Daily data refresh ────────────────────────────────────────────────
    // Runs once per day at ~4 AM local time. Fetches only the current NBA
    // season; BulkInserter uses ON CONFLICT DO NOTHING so re-runs are safe
    // and only new games are inserted.
    auto current_nba_season = []() -> int {
        auto now  = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm *lt = std::localtime(&t);
        int year  = lt->tm_year + 1900;
        int month = lt->tm_mon + 1;   // 1-based
        // NBA season start year: October onwards belongs to 'year', else 'year-1'
        return (month >= 10) ? year : year - 1;
    };

    // Seconds until next 4:00 AM local time.
    auto secs_until_4am = []() -> long {
        auto now  = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm *lt = std::localtime(&t);
        std::tm target = *lt;
        target.tm_hour = 4; target.tm_min = 0; target.tm_sec = 0;
        std::time_t t4 = std::mktime(&target);
        if (t4 <= t) t4 += 86400;   // already past 4 AM today → tomorrow
        return static_cast<long>(t4 - t);
    };

    std::jthread daily_refresher;
    if (!db_conn.empty()) {
        daily_refresher = std::jthread([&, current_nba_season, secs_until_4am](
                                           std::stop_token stop) {
            auto rlog = cortex::get_logger("refresh");
            // Wait until next 4 AM before the first run.
            long wait = secs_until_4am();
            rlog->info("Daily refresh scheduled in {:.1f} hours", wait / 3600.0);
            std::this_thread::sleep_for(std::chrono::seconds(wait));

            while (!stop.stop_requested()) {
                int season = current_nba_season();
                rlog->info("Daily refresh: loading season {} (regular + playoffs) …", season);
                try {
                    cortex::etl::NBAClient   client(150);
                    cortex::etl::BulkInserter inserter(db_conn);
                    int new_games = 0;

                    // Regular season
                    auto reg_ids = cortex::etl::NBAClient::game_ids_for_season(season, 1230, 2);
                    for (const auto& gid : reg_ids) {
                        if (stop.stop_requested()) break;
                        auto pbp = client.fetch_play_by_play(gid);
                        if (pbp) {
                            int64_t n = inserter.bulk_insert_play_by_play(*pbp, season);
                            if (n > 0) ++new_games;
                        }
                    }

                    // Playoffs (up to 400 IDs covers all rounds)
                    auto po_ids = cortex::etl::NBAClient::game_ids_for_season(season, 400, 4);
                    for (const auto& gid : po_ids) {
                        if (stop.stop_requested()) break;
                        auto pbp = client.fetch_play_by_play(gid);
                        if (pbp) {
                            int64_t n = inserter.bulk_insert_play_by_play(*pbp, season);
                            if (n > 0) ++new_games;
                        }
                    }

                    rlog->info("Daily refresh complete: {} new games inserted", new_games);

                    // Refresh materialized view so leaderboard picks up new data.
                    if (new_games > 0) {
                        try {
                            pqxx::connection mv_conn(db_conn);
                            pqxx::work mv_txn(mv_conn);
                            rlog->info("Refreshing player_game_stats materialized view…");
                            mv_txn.exec("REFRESH MATERIALIZED VIEW CONCURRENTLY player_game_stats");
                            mv_txn.commit();
                            rlog->info("Materialized view refreshed.");
                        } catch (const std::exception& e) {
                            rlog->warn("Materialized view refresh failed: {}", e.what());
                        }
                    }
                } catch (const std::exception& e) {
                    rlog->warn("Daily refresh failed: {}", e.what());
                }
                // Sleep until next 4 AM.
                std::this_thread::sleep_for(std::chrono::seconds(secs_until_4am()));
            }
        });
    }

    // Win probability model (optional — degrades gracefully if file missing)
    std::unique_ptr<WinProbModel> win_prob_model;
    try {
        win_prob_model = std::make_unique<WinProbModel>(model_path);
        log->info("Win probability model loaded from {}", model_path);
    } catch (const std::exception& e) {
        log->warn("Win probability model unavailable: {} — broadcasting without win_prob", e.what());
    }

    // ── Stream processor ──────────────────────────────────────────────────
    StreamProcessor proc(ring, accumulator);

    // ── HTTP server ───────────────────────────────────────────────────────
    HttpServer::Config server_cfg;
    server_cfg.port              = port;
    server_cfg.db_conn_str       = db_conn;
    server_cfg.www_root          = www_root;
    server_cfg.game_state_index  = &sim_index;
    server_cfg.elo_tracker       = &elo_tracker;

    HttpServer server(server_cfg, accumulator);
    g_server = &server;

    // Wire StreamProcessor events → WebSocket broadcast
    proc.start([&](const StreamEvent& ev) {
        if (ev.player_id <= 0) return;
        std::string gid(ev.game_id.data());

        // Compute win probability if model is available
        float wp = -1.0f;
        if (win_prob_model && win_prob_model->loaded()) {
            WinProbInput wpi{};
            wpi.score_diff     = static_cast<float>(ev.score_home - ev.score_away);
            int q = ev.period > 0 ? static_cast<int>(ev.period) : 1;
            wpi.quarter        = static_cast<float>(q);
            // Total seconds remaining = full periods left × 720s + current period clock
            int periods_left   = std::max(0, 4 - q);
            wpi.sec_remaining  = static_cast<float>(periods_left * 720 + ev.clock_secs);
            wpi.home_advantage = 1.0f;
            wpi.momentum       = 0.0f;  // simple broadcast: no rolling window here
            // Elo features from team strength ratings
            if (elo_tracker.built()) {
                wpi.elo_diff    = elo_tracker.elo_diff(ev.home_team_id, ev.away_team_id);
                wpi.elo_expected = EloTracker::expected_score(
                    elo_tracker.rating(ev.home_team_id),
                    elo_tracker.rating(ev.away_team_id));
            }
            try { wp = win_prob_model->predict(wpi); }
            catch (...) { wp = -1.0f; }
        }

        char buf[320];
        int n;
        if (wp >= 0.0f) {
            n = std::snprintf(buf, sizeof(buf),
                R"({"event":"play","game_id":"%s","player_id":%d,"action":%d,"shot_made":%s,"score_home":%d,"score_away":%d,"win_prob":%.4f})",
                gid.c_str(),
                ev.player_id,
                static_cast<int>(ev.action_type),
                ev.shot_made ? "true" : "false",
                ev.score_home,
                ev.score_away,
                static_cast<double>(wp));
        } else {
            n = std::snprintf(buf, sizeof(buf),
                R"({"event":"play","game_id":"%s","player_id":%d,"action":%d,"shot_made":%s,"score_home":%d,"score_away":%d})",
                gid.c_str(),
                ev.player_id,
                static_cast<int>(ev.action_type),
                ev.shot_made ? "true" : "false",
                ev.score_home,
                ev.score_away);
        }
        if (n > 0)
            server.broadcast(gid, std::string(buf, n));
    });

    // ── Live ingestion (optional) ─────────────────────────────────────────
    std::unique_ptr<LiveIngestor> ingestor;
    if (enable_live) {
        ingestor = std::make_unique<LiveIngestor>(ring, poll_interval_ms);
        ingestor->start();
        log->info("LiveIngestor started (poll interval: {}ms)", poll_interval_ms);
    } else {
        log->info("Live ingestion disabled (pass --live to enable)");
    }

    // ── Signals ───────────────────────────────────────────────────────────
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    log->info("Server ready. Ctrl+C to stop.");
    server.run();   // blocks until stop()

    if (ingestor) ingestor->stop();
    proc.stop();
    log->info("Shutdown complete.");
    return 0;
}
