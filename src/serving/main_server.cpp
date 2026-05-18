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
#include "common/Logger.hpp"

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

// Graceful shutdown
static std::atomic<bool> g_shutdown{false};
static HttpServer*        g_server  = nullptr;

static void handle_signal(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_release);
    if (g_server) g_server->stop();
}

int main(int argc, char** argv) {
    // ── Config from env / args ────────────────────────────────────────────
    uint16_t    port       = 8080;
    std::string db_conn    = "host=localhost port=5433 dbname=cortex";
    std::string redis_host = "127.0.0.1";
    int         redis_port = 6379;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc)
            db_conn = argv[++i];
        if (std::strcmp(argv[i], "--redis") == 0 && i + 1 < argc)
            redis_host = argv[++i];
    }

    auto log = cortex::get_logger("main");
    log->info("Cortex server starting on port {}", port);

    // ── Infrastructure ────────────────────────────────────────────────────
    RingBuffer<StreamEvent> ring(65536);
    StatAccumulator         accumulator;
    RedisCache              cache(redis_host, redis_port);

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
        // Build minimal JSON event for connected WebSocket clients
        char buf[256];
        std::string gid(ev.game_id.data());
        int n = std::snprintf(buf, sizeof(buf),
            R"({"event":"play","game_id":"%s","player_id":%d,"action":%d,"shot_made":%s,"score_home":%d,"score_away":%d})",
            gid.c_str(),
            ev.player_id,
            static_cast<int>(ev.action_type),
            ev.shot_made ? "true" : "false",
            ev.score_home,
            ev.score_away);
        if (n > 0)
            server.broadcast(gid, std::string(buf, n));
    });

    // ── Signals ───────────────────────────────────────────────────────────
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    log->info("Server ready. Ctrl+C to stop.");
    server.run();   // blocks until stop()

    proc.stop();
    log->info("Shutdown complete.");
    return 0;
}
