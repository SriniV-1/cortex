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
#include "etl/LiveIngestor.hpp"
#include "common/Logger.hpp"

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
    }

    auto log = cortex::get_logger("main");
    log->info("Cortex server starting on port {}", port);

    // ── Infrastructure ────────────────────────────────────────────────────
    RingBuffer<StreamEvent> ring(65536);
    StatAccumulator         accumulator;
    RedisCache              cache(redis_host, redis_port);

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
    server_cfg.port        = port;
    server_cfg.db_conn_str = db_conn;

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
