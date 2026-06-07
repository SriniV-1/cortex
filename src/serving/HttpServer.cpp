#include "serving/HttpServer.hpp"
#if defined(__APPLE__)
#  include "serving/KqueuePoller.hpp"
   using PlatformPoller = cortex::serving::KqueuePoller;
#else
#  include "serving/EpollPoller.hpp"
   using PlatformPoller = cortex::serving::EpollPoller;
#endif
#include "serving/Router.hpp"
#include "serving/handlers/health_handler.hpp"
#include "serving/handlers/stats_handler.hpp"
#include "serving/handlers/games_handler.hpp"
#include "serving/handlers/search_handler.hpp"
#include "serving/handlers/analytics_handler.hpp"
#include "serving/handlers/metrics_handler.hpp"
#include "serving/handlers/ws_handler.hpp"
#include "serving/handlers/docs_handler.hpp"
#include "serving/handlers/auth_handler.hpp"
#include "common/Logger.hpp"

#include "serving/ConnectionPool.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace cortex::serving {

// ── Helpers ────────────────────────────────────────────────────────────────

static void set_nonblock(int fd) {
    int f = ::fcntl(fd, F_GETFL, 0);
    if (f < 0 || ::fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("fcntl: ") + std::strerror(errno));
}

static void set_tcp_nodelay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// ── Route registration ────────────────────────────────────────────────────

void HttpServer::register_routes() {
    using namespace cortex::serving::handlers;
    router_.add("GET", "/health",                     handle_health);
    router_.add("GET", "/ready",                      handle_ready);
    router_.add("GET", "/metrics",                    handle_metrics);
    router_.add("GET", "/stats/:gameId",              handle_game_stats);
    router_.add("GET", "/players/:playerId/season",   handle_player_season);
    router_.add("GET", "/api/stats",                  handle_api_stats);
    router_.add("GET", "/api/leaderboard",            handle_leaderboard);
    router_.add("GET", "/api/players/search",         handle_search_players);
    router_.add("GET", "/api/games/search",           handle_search_games);
    router_.add("GET", "/api/events/search",          handle_search_events);
    router_.add("GET", "/api/games/recent",           handle_games_recent);
    router_.add("GET", "/api/scoreboard",             handle_scoreboard);
    router_.add("GET", "/api/elo",                    handle_elo_rankings);
    router_.add("GET", "/api/elo/history",            handle_elo_history);
    router_.add("GET", "/api/index/status",           handle_index_status);
    router_.add("GET", "/api/similar",                handle_similarity);
    router_.add("GET", "/live/:gameId",               handle_ws_upgrade);
    router_.add("GET", "/api/openapi.json",            handle_openapi);
    router_.add("GET", "/docs",                        handle_docs);
    router_.add("POST", "/api/auth/token",             handle_auth_token);
}

// ── Server lifecycle ──────────────────────────────────────────────────────

HttpServer::HttpServer(Config cfg, cortex::stream::StatAccumulator& accumulator)
    : cfg_(std::move(cfg))
    , accumulator_(accumulator)
    , poller_(std::make_unique<PlatformPoller>(1024))
{
    register_routes();

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(cfg_.port);
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(listen_fd_, cfg_.backlog) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    set_nonblock(listen_fd_);

    if (!cfg_.db_conn_str.empty()) {
        try { db_pool_ = std::make_unique<ConnectionPool>(cfg_.db_conn_str, 4); }
        catch (const std::exception& e) {
            auto log = cortex::get_logger("http");
            log->warn("DB pool unavailable: {}", e.what());
        }
    }
    cache_ = std::make_unique<RedisCache>(cfg_.redis_host, cfg_.redis_port);
}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

void HttpServer::run() {
    auto log = cortex::get_logger("http");
    log->info("HttpServer listening on port {}", cfg_.port);
    poller_->add(listen_fd_, IOEvent::Readable,
                 [this](int, IOEvent) { accept_connection(); });
    running_.store(true, std::memory_order_release);
    poller_->run();
    running_.store(false);
    log->info("HttpServer stopped");
}

void HttpServer::stop() { poller_->stop(); }

// ── Connection management ─────────────────────────────────────────────────

void HttpServer::accept_connection() {
    while (true) {
        struct sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        int cfd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (cfd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) return; return; }
        set_nonblock(cfd);
        set_tcp_nodelay(cfd);
        auto conn = std::make_unique<Connection>(cfd, *poller_);
        conn->accumulator       = &accumulator_;
        conn->db_pool           = db_pool_.get();
        conn->cache             = cache_.get();
        conn->www_root          = cfg_.www_root;
        conn->game_state_index  = cfg_.game_state_index;
        conn->elo_tracker       = cfg_.elo_tracker;
        conn->live_ingestor     = cfg_.live_ingestor;
        conn->rate_limiter      = &rate_limiter_;
        conn->redis_circuit_breaker = &cache_->circuit_breaker();
        conn->jwt_secret        = cfg_.jwt_secret;
        conn->api_key           = cfg_.api_key;
        conn->router            = &router_;
        if (peer.ss_family == AF_INET) {
            char ip[INET_ADDRSTRLEN]{};
            auto* sa4 = reinterpret_cast<struct sockaddr_in*>(&peer);
            ::inet_ntop(AF_INET, &sa4->sin_addr, ip, sizeof(ip));
            conn->client_ip = ip;
        }
        Connection* raw = conn.get();
        { std::lock_guard lock(conn_mu_); conns_[cfd] = std::move(conn); }
        poller_->add(cfd, IOEvent::Readable,
                     [this, raw](int, IOEvent ev) {
                         if (ev & IOEvent::Readable) raw->on_readable();
                         if (ev & IOEvent::Writable) raw->on_writable();
                         if ((ev & IOEvent::Error) || (ev & IOEvent::HangUp) || raw->closed())
                             reap_closed();
                     });
    }
}

void HttpServer::reap_closed() {
    std::lock_guard lock(conn_mu_);
    for (auto it = conns_.begin(); it != conns_.end(); )
        it = it->second->closed() ? conns_.erase(it) : ++it;
}

void HttpServer::broadcast(const std::string& game_id, std::string payload) {
    std::lock_guard lock(conn_mu_);
    for (auto& [fd, conn] : conns_)
        if (conn->is_ws() && conn->subscribed_game() == game_id)
            conn->ws_send(payload);
}

} // namespace cortex::serving
