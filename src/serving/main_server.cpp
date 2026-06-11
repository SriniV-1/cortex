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
#include "distributed/Coordinator.hpp"
#include "distributed/IngestorNode.hpp"
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
#include <fstream>
#include <filesystem>

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
    std::string log_file;

    // Auth secrets. When jwt_secret is non-empty the HTTP server enforces JWT
    // auth + RBAC on all non-exempt routes; api_key gates token issuance via
    // POST /api/auth/token. Both default to the environment so secrets are
    // never baked into argv (visible in `ps`). CLI flags override env.
    std::string jwt_secret = []{ const char* v = std::getenv("CORTEX_JWT_SECRET"); return v ? std::string(v) : std::string(); }();
    std::string api_key    = []{ const char* v = std::getenv("CORTEX_API_KEY");    return v ? std::string(v) : std::string(); }();

    // Distributed cluster flags.
    std::string mode = "standalone";   // standalone | coordinator | worker
    std::string coordinator_addr;      // worker mode: coordinator gRPC address
    int         grpc_port = 50051;     // coordinator mode: gRPC listen port
    int         capacity  = 20;        // worker mode: max games

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
        if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_file = argv[++i];
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode = argv[++i];
        if (std::strcmp(argv[i], "--coordinator") == 0 && i + 1 < argc)
            coordinator_addr = argv[++i];
        if (std::strcmp(argv[i], "--grpc-port") == 0 && i + 1 < argc)
            grpc_port = std::atoi(argv[++i]);
        if (std::strcmp(argv[i], "--capacity") == 0 && i + 1 < argc)
            capacity = std::atoi(argv[++i]);
        if (std::strcmp(argv[i], "--jwt-secret") == 0 && i + 1 < argc)
            jwt_secret = argv[++i];
        if (std::strcmp(argv[i], "--api-key") == 0 && i + 1 < argc)
            api_key = argv[++i];
    }

    auto log = cortex::get_logger("main");
    log->info("Cortex server starting on port {} (mode={})", port, mode);

    // ── Distributed modes ────────────────────────────────────────────────
    if (mode == "coordinator") {
        distributed::Coordinator::Config coord_cfg;
        coord_cfg.grpc_address = "0.0.0.0:" + std::to_string(grpc_port);
        auto coordinator = std::make_unique<distributed::Coordinator>(coord_cfg);
        coordinator->start();
        log->info("Coordinator mode — gRPC on port {}, HTTP on port {}", grpc_port, port);

        // Still run the HTTP server for dashboard + cluster status API.
        // (Falls through to normal HTTP server setup below.)
    }

    if (mode == "worker") {
        if (coordinator_addr.empty()) {
            log->error("Worker mode requires --coordinator <host:port>");
            return 1;
        }

        distributed::IngestorNode::Config worker_cfg;
        worker_cfg.coordinator_address = coordinator_addr;
        worker_cfg.host = "0.0.0.0";
        worker_cfg.http_port = port;
        worker_cfg.capacity = capacity;
        auto worker_node = std::make_unique<distributed::IngestorNode>(worker_cfg);
        worker_node->start();

        log->info("Worker mode — coordinator={}, capacity={}", coordinator_addr, capacity);

        // Block until shutdown signal.
        struct sigaction sa{};
        sa.sa_handler = handle_signal;
        ::sigaction(SIGINT,  &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        log->info("Worker {} ready. Ctrl+C to stop.", worker_node->worker_id());
        while (!g_shutdown.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        worker_node->stop();
        log->info("Worker shutdown complete.");
        return 0;
    }

    // ── Infrastructure ────────────────────────────────────────────────────
    RingBuffer<StreamEvent> ring(65536);
    StatAccumulator         accumulator;
    RedisCache              cache(redis_host, redis_port);

    // ── Similarity index (built in background — HTTP server starts immediately) ──
    GameStateIndex sim_index;
    std::thread   sim_builder;
    if (!db_conn.empty()) {
        sim_builder = std::thread([&]() {
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
    std::thread elo_builder;
    if (!db_conn.empty()) {
        elo_builder = std::thread([&]() {
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

    // Shared stop flag for background threads (replaces std::stop_token).
    std::atomic<bool> bg_stop{false};

    // Interruptible sleep: sleeps in 1-second increments, checking the stop flag.
    auto interruptible_sleep = [&bg_stop](long seconds) {
        for (long i = 0; i < seconds && !bg_stop.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    };

    // Truncate log file to the last N lines to prevent unbounded growth.
    auto truncate_log = [&log_file](int keep_lines = 1000) {
        if (log_file.empty()) return;
        try {
            if (!std::filesystem::exists(log_file)) return;
            auto size = std::filesystem::file_size(log_file);
            if (size < 512 * 1024) return;  // only bother if > 512 KB

            std::ifstream in(log_file);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line)) lines.push_back(std::move(line));
            in.close();

            if (static_cast<int>(lines.size()) <= keep_lines) return;

            std::ofstream out(log_file, std::ios::trunc);
            for (size_t i = lines.size() - keep_lines; i < lines.size(); ++i)
                out << lines[i] << '\n';
        } catch (...) {}
    };

    // Helper: run the full ETL pipeline for a single game (boxscore + play-by-play).
    // Returns true if a new game was inserted.
    auto load_game_full = [](cortex::etl::NBAClient& client,
                             cortex::etl::BulkInserter& inserter,
                             const std::string& gid, int season,
                             std::shared_ptr<spdlog::logger> lg) -> bool {
        // Step 1: boxscore → teams, players, game metadata
        auto bs = client.fetch_boxscore(gid);
        if (bs) {
            inserter.ensure_team(bs->home_team);
            inserter.ensure_team(bs->away_team);
            inserter.upsert_game(bs->game);
            auto all_players = bs->home_players;
            all_players.insert(all_players.end(),
                               bs->away_players.begin(),
                               bs->away_players.end());
            inserter.upsert_players(all_players);
        }
        // Step 2: play-by-play → play_events
        auto pbp = client.fetch_play_by_play(gid);
        if (pbp) {
            int64_t n = inserter.bulk_insert_play_by_play(*pbp, season);
            return n > 0;
        }
        return false;
    };

    // Helper: refresh materialized view and rebuild Elo after new games are loaded.
    auto post_load_refresh = [&db_conn, &elo_tracker](
            std::shared_ptr<spdlog::logger> lg) {
        try {
            pqxx::connection mv_conn(db_conn);
            pqxx::work mv_txn(mv_conn);
            lg->info("Refreshing player_game_stats materialized view…");
            mv_txn.exec("REFRESH MATERIALIZED VIEW CONCURRENTLY player_game_stats");
            mv_txn.commit();
            lg->info("Materialized view refreshed.");
        } catch (const std::exception& e) {
            lg->warn("Materialized view refresh failed: {}", e.what());
        }
        // Rebuild Elo ratings so rankings reflect newly loaded games.
        try {
            pqxx::connection elo_conn(db_conn);
            elo_tracker.build_from_db(elo_conn);
            elo_tracker.save_to_db(elo_conn);
            lg->info("Elo ratings rebuilt.");
        } catch (const std::exception& e) {
            lg->warn("Elo rebuild failed: {}", e.what());
        }
    };

    // ── Daily refresh: runs immediately on startup, then every day at 4 AM ──
    std::thread daily_refresher;
    if (!db_conn.empty()) {
        daily_refresher = std::thread([&, current_nba_season, secs_until_4am,
                                        interruptible_sleep, truncate_log,
                                        load_game_full, post_load_refresh]() {
            auto rlog = cortex::get_logger("refresh");
            bool first_run = true;

            while (!bg_stop.load(std::memory_order_acquire)) {
                if (first_run) {
                    rlog->info("Startup refresh: checking for new games…");
                    first_run = false;
                } else {
                    long wait = secs_until_4am();
                    rlog->info("Next daily refresh in {:.1f} hours", wait / 3600.0);
                    interruptible_sleep(wait);
                    if (bg_stop.load(std::memory_order_acquire)) break;
                }

                int season = current_nba_season();
                rlog->info("Loading season {} (regular + playoffs) …", season);
                try {
                    cortex::etl::NBAClient   client(150);
                    cortex::etl::BulkInserter inserter(db_conn);
                    int new_games = 0;
                    int consecutive_miss = 0;
                    int seen_any = 0;  // games loaded or newly inserted

                    // Regular season
                    auto reg_ids = cortex::etl::NBAClient::game_ids_for_season(season, 1230, 2);
                    for (const auto& gid : reg_ids) {
                        if (bg_stop.load(std::memory_order_acquire)) break;
                        if (inserter.is_game_loaded(gid)) {
                            consecutive_miss = 0;
                            ++seen_any;
                            // Re-fetch games stored before they were final so a
                            // stuck pre-final / 0-0 score self-corrects on refresh.
                            if (!inserter.is_game_final(gid))
                                load_game_full(client, inserter, gid, season, rlog);
                            continue;
                        }
                        if (load_game_full(client, inserter, gid, season, rlog))
                            { ++new_games; ++seen_any; consecutive_miss = 0; }
                        else
                            { if (++consecutive_miss >= 30 && seen_any > 0) break; }
                    }

                    // Playoffs
                    consecutive_miss = 0;
                    seen_any = 0;
                    auto po_ids = cortex::etl::NBAClient::game_ids_for_season(season, 500, 4);
                    for (const auto& gid : po_ids) {
                        if (bg_stop.load(std::memory_order_acquire)) break;
                        if (inserter.is_game_loaded(gid)) {
                            consecutive_miss = 0;
                            ++seen_any;
                            // Re-fetch games stored before they were final so a
                            // stuck pre-final / 0-0 score self-corrects on refresh.
                            if (!inserter.is_game_final(gid))
                                load_game_full(client, inserter, gid, season, rlog);
                            continue;
                        }
                        if (load_game_full(client, inserter, gid, season, rlog))
                            { ++new_games; ++seen_any; consecutive_miss = 0; }
                        else
                            { if (++consecutive_miss >= 120 && seen_any > 0) break; }
                    }

                    rlog->info("Refresh complete: {} new games inserted", new_games);
                    if (new_games > 0) post_load_refresh(rlog);
                } catch (const std::exception& e) {
                    rlog->warn("Refresh failed: {}", e.what());
                }
                truncate_log(1000);
            }
        });
    }

    // Game completion callback — wired below after LiveIngestor is created.

    // Win probability model (optional — degrades gracefully if file missing)
    std::unique_ptr<WinProbModel> win_prob_model;
    try {
        win_prob_model = std::make_unique<WinProbModel>(model_path);
        log->info("Win probability model loaded from {}", model_path);
    } catch (const std::exception& e) {
        log->warn("Win probability model unavailable: {} — broadcasting without win_prob", e.what());
    }

    // ── Live ingestion (optional — create early so HttpServer can reference it)
    std::unique_ptr<LiveIngestor> ingestor;
    if (enable_live) {
        ingestor = std::make_unique<LiveIngestor>(ring, poll_interval_ms);

        // When a game ends, persist it to the DB on the next poll cycle (~30s).
        if (!db_conn.empty()) {
            ingestor->on_game_complete(
                [&db_conn, &elo_tracker, current_nba_season,
                 load_game_full, post_load_refresh](const std::string& game_id) {
                    auto clog = cortex::get_logger("completion");
                    try {
                        cortex::etl::NBAClient    client(50);
                        cortex::etl::BulkInserter inserter(db_conn);
                        if (inserter.is_game_loaded(game_id)) return;

                        int season = current_nba_season();
                        if (load_game_full(client, inserter, game_id, season, clog)) {
                            clog->info("Persisted completed game {}", game_id);
                            post_load_refresh(clog);
                        }
                    } catch (const std::exception& e) {
                        clog->warn("Failed to persist game {}: {}", game_id, e.what());
                    }
                });
        }
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
    server_cfg.live_ingestor     = ingestor.get();
    server_cfg.jwt_secret        = jwt_secret;
    server_cfg.api_key           = api_key;

    if (jwt_secret.empty()) {
        log->warn("AUTH DISABLED: no JWT secret configured — all endpoints are "
                  "open, including POST/admin routes. Set CORTEX_JWT_SECRET (and "
                  "CORTEX_API_KEY for token issuance) before exposing this server "
                  "beyond localhost.");
    } else {
        log->info("Auth enabled: JWT/RBAC enforced on non-exempt routes.");
        if (api_key.empty())
            log->warn("CORTEX_API_KEY unset — POST /api/auth/token cannot issue "
                      "tokens until it is configured.");
    }

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

        char buf[400];
        int n;
        if (wp >= 0.0f) {
            n = std::snprintf(buf, sizeof(buf),
                R"({"event":"play","game_id":"%s","player_id":%d,"action":%d,"shot_made":%s,"score_home":%d,"score_away":%d,"period":%d,"clock_secs":%d,"win_prob":%.4f})",
                gid.c_str(),
                ev.player_id,
                static_cast<int>(ev.action_type),
                ev.shot_made ? "true" : "false",
                ev.score_home,
                ev.score_away,
                static_cast<int>(ev.period),
                static_cast<int>(ev.clock_secs),
                static_cast<double>(wp));
        } else {
            n = std::snprintf(buf, sizeof(buf),
                R"({"event":"play","game_id":"%s","player_id":%d,"action":%d,"shot_made":%s,"score_home":%d,"score_away":%d,"period":%d,"clock_secs":%d})",
                gid.c_str(),
                ev.player_id,
                static_cast<int>(ev.action_type),
                ev.shot_made ? "true" : "false",
                ev.score_home,
                ev.score_away,
                static_cast<int>(ev.period),
                static_cast<int>(ev.clock_secs));
        }
        if (n > 0)
            server.broadcast(gid, std::string(buf, n));
    });

    // ── Periodic stat eviction (reclaim memory for finished games) ─────────
    std::thread stat_evictor([&, interruptible_sleep]() {
        while (!bg_stop.load(std::memory_order_acquire)) {
            interruptible_sleep(600);  // 10 minutes
            if (bg_stop.load(std::memory_order_acquire)) break;
            accumulator.evict_stale(std::chrono::seconds(4 * 3600));
        }
    });

    // ── Start live ingestion ────────────────────────────────────────────
    if (ingestor) {
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

    // ── Graceful shutdown — stop all background threads before locals destruct ──
    log->info("Shutting down…");
    if (ingestor) ingestor->stop();
    proc.stop();

    // Signal all background threads to stop, then join in reverse start order.
    bg_stop.store(true, std::memory_order_release);
    if (stat_evictor.joinable())    stat_evictor.join();
    if (daily_refresher.joinable()) daily_refresher.join();
    if (elo_builder.joinable())       elo_builder.join();
    if (sim_builder.joinable())       sim_builder.join();

    log->info("Shutdown complete.");
    return 0;
}
