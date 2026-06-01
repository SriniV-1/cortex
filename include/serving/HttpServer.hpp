#pragma once
// HttpServer — HTTP/1.1 + WebSocket server using KqueuePoller.
//
// Architecture:
//   - Main acceptor loop runs on the poller thread (registered on the listen fd)
//   - Each accepted connection gets a Connection object and its own poller registration
//   - HTTP parsing via llhttp; WebSocket frames hand-rolled (text only, no fragmentation)
//   - WebSocket sessions are fan-in to StreamProcessor callback and fan-out to clients
//
// Endpoints:
//   GET /health                     → 200 OK + JSON status
//   GET /stats/{gameId}             → live game stat snapshot (JSON)
//   GET /players/{playerId}/season  → player season totals (JSON)
//   GET /live/{gameId}              → WebSocket upgrade → streaming events
//   GET /metrics                    → Prometheus text exposition
//
// Thread model:
//   Server thread (kqueue loop) — handles all I/O
//   Broadcaster thread — drains outbound queue and writes to WebSocket clients
//   (No user-thread blocking; DB queries dispatched synchronously from server thread
//    since they are indexed and sub-millisecond)

#include "IOPoller.hpp"
#include "serving/RateLimiter.hpp"
#include "serving/RedisCache.hpp"
#include "serving/Router.hpp"
#include "stream/StatAccumulator.hpp"
#include "stream/StreamEvent.hpp"

// Forward-declare to avoid pulling in pqxx + arm_neon headers into every TU.
namespace cortex::analytics { class GameStateIndex; class EloTracker; }
namespace cortex::etl { class LiveIngestor; }

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>

#include <llhttp.h>

namespace pqxx { class connection; }

namespace cortex::serving {

// ── Outbound WebSocket message ─────────────────────────────────────────────
struct WsMessage {
    std::string payload;  // already JSON-encoded
};

// ── Per-connection state ───────────────────────────────────────────────────
class Connection {
public:
    enum class State { Http, WebSocket, Closing };

    explicit Connection(int fd, IOPoller& poller);
    ~Connection();

    // Called by poller on readable event
    void on_readable();
    // Called by poller on writable event
    void on_writable();

    // Enqueue a WebSocket text frame for delivery (thread-safe)
    void ws_send(std::string payload);

    int  fd()    const noexcept { return fd_; }
    bool closed() const noexcept { return state_ == State::Closing; }
    bool is_ws()  const noexcept { return state_ == State::WebSocket; }

    // Game id this WebSocket is subscribed to (empty = not subscribed)
    const std::string& subscribed_game() const noexcept { return subscribed_game_; }

    // Injected dependencies — set by HttpServer before registering with poller
    cortex::stream::StatAccumulator*         accumulator        = nullptr;
    pqxx::connection*                        db_conn            = nullptr;
    RedisCache*                              cache              = nullptr;
    std::string                              www_root;
    const cortex::analytics::GameStateIndex* game_state_index   = nullptr;
    const cortex::analytics::EloTracker*    elo_tracker        = nullptr;
    const cortex::etl::LiveIngestor*         live_ingestor      = nullptr;
    RateLimiter*                             rate_limiter       = nullptr;
    std::string                              client_ip;
    const Router*                            router             = nullptr;

    // Called from llhttp static callbacks — must remain accessible
    void process_http_request(const std::string& method,
                               const std::string& path,
                               const std::string& headers_raw,
                               const std::string& body);

    // WebSocket upgrade — public so ws_handler can invoke it via ServerContext.
    void upgrade_to_websocket(const std::string& key, const std::string& game_id);

private:
    void close();

    // HTTP response — only called from within Connection methods.
    void send_response(int status, const std::string& content_type,
                       const std::string& body, bool keep_alive = false);

    // WebSocket helpers
    void  ws_parse_incoming();
    void  ws_flush_outbound();
    std::string ws_encode_frame(const std::string& payload);

public:
    // ── Implementation fields ────────────────────────────────────────────
    // Accessible to llhttp static callbacks in Connection.cpp.
    // Not part of the external API.

    int         fd_;
    IOPoller&   poller_;
    State       state_              = State::Http;
    bool        close_after_flush_  = false;  // set by send_response; actual close on flush

    // HTTP parser (llhttp)
    std::unique_ptr<llhttp_t>          parser_;
    std::unique_ptr<llhttp_settings_t> parser_settings_;

    std::string read_buf_;   // raw bytes from socket
    std::string write_buf_;  // bytes pending write

    // HTTP request assembly (written by llhttp callbacks)
    std::string req_method_;
    std::string req_url_;
    std::string req_headers_raw_;
    std::string req_body_;
    std::string cur_header_field_;
    std::string cur_header_value_;
    bool        headers_complete_ = false;

    // WebSocket
    std::string             subscribed_game_;
    std::mutex              ws_out_mu_;
    std::queue<std::string> ws_out_queue_;  // encoded frames
    static constexpr size_t kMaxWsQueueFrames = 1024;
};

// ── HttpServer ─────────────────────────────────────────────────────────────
class HttpServer {
public:
    struct Config {
        std::string host        = "0.0.0.0";
        uint16_t    port        = 8080;
        int         backlog     = 128;
        std::string db_conn_str;                    // libpqxx connection string
        std::string redis_host  = "127.0.0.1";
        int         redis_port  = 6379;
        std::string www_root    = "www";            // directory for static files
        const cortex::analytics::GameStateIndex* game_state_index = nullptr;
        const cortex::analytics::EloTracker*    elo_tracker      = nullptr;
        const cortex::etl::LiveIngestor*         live_ingestor    = nullptr;
    };

    explicit HttpServer(Config cfg,
                        cortex::stream::StatAccumulator& accumulator);
    ~HttpServer();

    // Start listening and run the event loop (blocks until stop()).
    void run();

    // Stop the server. Thread-safe.
    void stop();

    // Broadcast a JSON payload to all WebSocket clients subscribed to game_id.
    // Called from the stream processor callback (any thread).
    void broadcast(const std::string& game_id, std::string payload);

    bool running() const noexcept { return running_.load(); }

private:
    void accept_connection();
    void reap_closed();
    void register_routes();

    Config                                   cfg_;
    cortex::stream::StatAccumulator&         accumulator_;
    std::unique_ptr<IOPoller>                poller_;
    int                                      listen_fd_ = -1;
    std::atomic<bool>                        running_{false};

    // Route dispatcher
    Router                                   router_;

    // Connections keyed by fd
    mutable std::mutex                              conn_mu_;
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;

    // Shared DB connection (all queries from poller thread — no lock needed)
    std::unique_ptr<pqxx::connection>        db_;
    // Redis cache (1-min TTL on aggregation results)
    std::unique_ptr<RedisCache>              cache_;
    // Token bucket rate limiter
    RateLimiter                              rate_limiter_;
};

} // namespace cortex::serving
