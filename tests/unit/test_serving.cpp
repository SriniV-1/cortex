// Unit tests for Phase 3 serving layer components.
// Tests: KqueuePoller, HttpServer (HTTP endpoints), WebSocket handshake.
//
// Note: WebSocket and Redis integration tests require a running server;
// those are skipped unless CORTEX_RUN_SERVING_TESTS=1.

#include "serving/KqueuePoller.hpp"
#include "serving/HttpServer.hpp"
#include "serving/RedisCache.hpp"
#include "stream/StatAccumulator.hpp"
#include "stream/StreamEvent.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

using namespace cortex::serving;
using namespace cortex::stream;

// ── KqueuePoller ──────────────────────────────────────────────────────────

TEST(KqueuePoller, ConstructAndStop) {
    KqueuePoller poller;
    std::atomic<bool> stopped{false};

    std::jthread t([&] {
        poller.run();
        stopped.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    poller.stop();
    t.join();
    EXPECT_TRUE(stopped.load());
}

TEST(KqueuePoller, WakeupUnblocks) {
    KqueuePoller poller;
    std::jthread t([&] { poller.run(); });

    // wakeup() + stop() both unblock run()
    poller.wakeup();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    poller.stop();
    t.join();
    SUCCEED();
}

TEST(KqueuePoller, PipeReadableEvent) {
    KqueuePoller poller;
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);

    std::atomic<bool> got_readable{false};

    poller.add(pipefd[0], IOEvent::Readable,
               [&](int /*fd*/, IOEvent ev) {
                   if (ev & IOEvent::Readable) {
                       got_readable.store(true);
                       poller.stop();
                   }
               });

    std::jthread t([&] { poller.run(); });

    // Write to pipe to trigger readable event
    char b = 'x';
    ::write(pipefd[1], &b, 1);

    t.join();
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    EXPECT_TRUE(got_readable.load());
}

// ── HttpServer helpers ────────────────────────────────────────────────────

// Make a simple blocking TCP connection and send/receive
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

static std::string recv_response(int fd, size_t max_bytes = 4096) {
    std::string resp;
    char buf[1024];
    while (resp.size() < max_bytes) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
        // Stop once we've received the full HTTP response headers + body
        if (resp.find("\r\n\r\n") != std::string::npos) {
            // Check Content-Length to know when body is complete
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
    return resp;
}

// ── HttpServer tests ──────────────────────────────────────────────────────

class HttpServerTest : public ::testing::Test {
protected:
    static constexpr uint16_t kPort = 18080;

    void SetUp() override {
        HttpServer::Config cfg;
        cfg.port = kPort;
        // No DB — /players endpoint will return 500, that's OK for these tests
        server_ = std::make_unique<HttpServer>(cfg, acc_);
        server_thread_ = std::jthread([this] { server_->run(); });
        // Wait for server to start
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server_->running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    void TearDown() override {
        server_->stop();
        server_thread_.join();
    }

    StatAccumulator acc_;
    std::unique_ptr<HttpServer> server_;
    std::jthread server_thread_;
};

TEST_F(HttpServerTest, HealthEndpoint) {
    int fd = tcp_connect(kPort);
    ASSERT_GE(fd, 0);

    std::string req = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string resp = recv_response(fd);
    ::close(fd);

    EXPECT_NE(resp.find("200"), std::string::npos);
    EXPECT_NE(resp.find("ok"), std::string::npos);
}

TEST_F(HttpServerTest, StatsEndpoint) {
    // Seed accumulator with a game event
    StreamEvent ev{};
    ev.player_id   = 23;
    ev.action_type = ActionType::Shot2pt;
    ev.shot_made   = true;
    ev.score_home  = 2;
    ev.score_away  = 0;
    const char* gid = "0022300001";
    std::copy(gid, gid + 11, ev.game_id.begin());
    acc_.process(ev);

    int fd = tcp_connect(kPort);
    ASSERT_GE(fd, 0);

    std::string req = "GET /stats/0022300001 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string resp = recv_response(fd);
    ::close(fd);

    EXPECT_NE(resp.find("200"), std::string::npos);
    EXPECT_NE(resp.find("0022300001"), std::string::npos);
    EXPECT_NE(resp.find("score_home"), std::string::npos);
}

TEST_F(HttpServerTest, MetricsEndpoint) {
    int fd = tcp_connect(kPort);
    ASSERT_GE(fd, 0);

    std::string req = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string resp = recv_response(fd, 8192);
    ::close(fd);

    EXPECT_NE(resp.find("200"), std::string::npos);
    EXPECT_NE(resp.find("cortex_events_processed"), std::string::npos);
}

TEST_F(HttpServerTest, NotFound) {
    int fd = tcp_connect(kPort);
    ASSERT_GE(fd, 0);

    std::string req = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string resp = recv_response(fd);
    ::close(fd);

    EXPECT_NE(resp.find("404"), std::string::npos);
}

TEST_F(HttpServerTest, WebSocketUpgradeHandshake) {
    int fd = tcp_connect(kPort);
    ASSERT_GE(fd, 0);

    // Minimal valid WebSocket upgrade request
    std::string req =
        "GET /live/0022300001 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    ::send(fd, req.data(), req.size(), 0);

    // Read response — should be 101 Switching Protocols
    char buf[1024];
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);

    ASSERT_GT(n, 0);
    std::string resp(buf, n);
    EXPECT_NE(resp.find("101"), std::string::npos);
    EXPECT_NE(resp.find("websocket"), std::string::npos);
    EXPECT_NE(resp.find("Sec-WebSocket-Accept"), std::string::npos);
}

// ── RedisCache tests ──────────────────────────────────────────────────────

TEST(RedisCache, ConnectAndBasicOps) {
    RedisCache cache;
    if (!cache.connected()) {
        GTEST_SKIP() << "Redis not available";
    }

    EXPECT_TRUE(cache.set_with_status("cortex_test_key", "hello", std::chrono::seconds{5}));
    auto val = cache.get("cortex_test_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello");

    cache.del("cortex_test_key");
    EXPECT_FALSE(cache.get("cortex_test_key").has_value());
}

TEST(RedisCache, MissingKeyReturnsNullopt) {
    RedisCache cache;
    if (!cache.connected()) {
        GTEST_SKIP() << "Redis not available";
    }
    EXPECT_FALSE(cache.get("cortex_definitely_missing_key_xyz").has_value());
}
