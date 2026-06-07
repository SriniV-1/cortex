// WebSocket load test — Phase 3 acceptance criterion.
//
// Measures broadcast latency: time from StreamProcessor emitting an event
// to the last of N_CLIENTS WebSocket subscribers receiving the frame.
//
// Target: p99 < 20ms with 1000 concurrent WebSocket clients.
//
// Method:
//   - Embed the full server stack in-process (StatAccumulator + StreamProcessor
//     + HttpServer) on a dedicated server thread.
//   - Spawn N_CLIENTS client threads; each does the WS handshake then blocks
//     on recv waiting for text frames.
//   - Producer loop: inject ROUNDS events one at a time, timestamp each inject,
//     wait until all clients ack receipt via atomic counter, record broadcast
//     latency = now() − inject_time.
//   - Report p50/p95/p99/max of broadcast latency over all ROUNDS.

#include "serving/HttpServer.hpp"
#include "stream/RingBuffer.hpp"
#include "stream/StreamEvent.hpp"
#include "stream/StreamProcessor.hpp"
#include "stream/StatAccumulator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace cortex::stream;
using namespace cortex::serving;
using Clock = std::chrono::steady_clock;

// ── Config ─────────────────────────────────────────────────────────────────
static constexpr int      N_CLIENTS  = 1000;
static constexpr int      ROUNDS     = 50;    // broadcast rounds to measure
static constexpr int      WARMUP     = 5;     // rounds to discard
static constexpr uint16_t PORT       = 19090;
static const std::string  GAME_ID    = "0022300001";

// ── WebSocket client helpers ───────────────────────────────────────────────

static int ws_connect(uint16_t port) {
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

// Send HTTP upgrade request and consume the 101 response.
// Returns true on success.
static bool ws_upgrade(int fd, const std::string& game_id) {
    std::string req =
        "GET /live/" + game_id + " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (::send(fd, req.data(), req.size(), 0) < 0) return false;

    // Read until we see "\r\n\r\n" (end of 101 headers)
    std::string resp;
    char buf[512];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        resp.append(buf, n);
    }
    return resp.find("101") != std::string::npos;
}

// Receive one WebSocket text frame. Blocks until a complete frame arrives.
// Returns payload, or "" on error/close.
static std::string ws_recv_frame(int fd) {
    // Read at least 2 header bytes
    unsigned char header[10];
    if (::recv(fd, header, 2, MSG_WAITALL) != 2) return "";

    size_t payload_len = header[1] & 0x7F;
    if (payload_len == 126) {
        if (::recv(fd, header + 2, 2, MSG_WAITALL) != 2) return "";
        payload_len = (static_cast<size_t>(header[2]) << 8) | header[3];
    } else if (payload_len == 127) {
        if (::recv(fd, header + 2, 8, MSG_WAITALL) != 8) return "";
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | header[2 + i];
    }

    if (payload_len == 0) return "";

    std::string payload(payload_len, '\0');
    ssize_t got = ::recv(fd, payload.data(), payload_len, MSG_WAITALL);
    if (got != static_cast<ssize_t>(payload_len)) return "";
    return payload;
}

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::printf("══════════════════════════════════════════════════════\n");
    std::printf("  Cortex WebSocket Broadcast Load Test\n");
    std::printf("  Clients: %d  |  Rounds: %d  |  Target: p99 < 20ms\n",
                N_CLIENTS, ROUNDS);
    std::printf("══════════════════════════════════════════════════════\n\n");

    // ── Server stack ───────────────────────────────────────────────────
    RingBuffer<StreamEvent> ring(65536);
    StatAccumulator         accumulator;

    HttpServer::Config cfg;
    cfg.port = PORT;
    // No DB or Redis needed for load test
    HttpServer server(cfg, accumulator);

    StreamProcessor proc(ring, accumulator);

    // Wire processor → broadcast
    proc.start([&](const StreamEvent& ev) {
        std::string gid(ev.game_id.data());
        char buf[256];
        int n = std::snprintf(buf, sizeof(buf),
            R"({"event":"play","game_id":"%s","player_id":%d,"action":%d})",
            gid.c_str(), ev.player_id, static_cast<int>(ev.action_type));
        if (n > 0) server.broadcast(gid, std::string(buf, n));
    });

    std::thread server_thread([&] { server.run(); });

    // Wait for server ready
    while (!server.running())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::printf("  Server ready on port %d\n", PORT);

    // ── Spawn client threads ───────────────────────────────────────────
    struct ClientState {
        int  fd         = -1;
        bool connected  = false;
        // Latency samples in nanoseconds (one per broadcast round)
        std::vector<int64_t> samples;
        // Signaling: broadcast_time is set by producer, recv_time by client
        std::atomic<Clock::rep> broadcast_ns{0};
    };

    std::vector<ClientState> clients(N_CLIENTS);
    // recv_count[round]: how many clients have received the broadcast for that round
    std::vector<std::atomic<int>> recv_count(ROUNDS + WARMUP);
    for (auto& a : recv_count) a.store(0);

    std::atomic<int> connected_count{0};

    // Client threads: connect, handshake, then loop receiving frames
    std::vector<std::thread> client_threads;
    client_threads.reserve(N_CLIENTS);

    for (int i = 0; i < N_CLIENTS; ++i) {
        client_threads.emplace_back([&, i] {
            ClientState& cs = clients[i];
            cs.fd = ws_connect(PORT);
            if (cs.fd < 0) return;
            if (!ws_upgrade(cs.fd, GAME_ID)) { ::close(cs.fd); cs.fd = -1; return; }
            cs.connected = true;
            connected_count.fetch_add(1, std::memory_order_release);

            for (int r = 0; r < ROUNDS + WARMUP; ++r) {
                std::string frame = ws_recv_frame(cs.fd);
                if (frame.empty()) break;
                auto now = Clock::now().time_since_epoch().count();
                // Record receipt, signal producer
                recv_count[r].fetch_add(1, std::memory_order_release);
                (void)now;
            }
            ::close(cs.fd);
        });
    }

    // Wait for all clients to connect and handshake
    std::printf("  Connecting %d clients", N_CLIENTS);
    auto connect_deadline = Clock::now() + std::chrono::seconds(30);
    while (connected_count.load(std::memory_order_acquire) < N_CLIENTS) {
        if (Clock::now() > connect_deadline) {
            std::printf("\n  TIMEOUT: only %d/%d clients connected\n",
                        connected_count.load(), N_CLIENTS);
            server.stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (connected_count.load() % 100 == 0 && connected_count.load() > 0)
            std::printf(".");
    }
    std::printf("\n  All %d clients connected.\n\n", N_CLIENTS);

    // ── Broadcast rounds ──────────────────────────────────────────────
    auto make_event = [&](int round) {
        StreamEvent ev{};
        ev.player_id   = (round % 10) + 1;
        ev.action_type = ActionType::Shot2pt;
        ev.shot_made   = true;
        ev.score_home  = static_cast<int16_t>(round % 120);
        ev.score_away  = static_cast<int16_t>(round % 100);
        const char* gid = GAME_ID.c_str();
        std::copy(gid, gid + 11, ev.game_id.begin());
        return ev;
    };

    std::vector<double> latencies_ms;  // broadcast latency per round (excluding warmup)
    latencies_ms.reserve(ROUNDS);

    for (int r = 0; r < ROUNDS + WARMUP; ++r) {
        auto t_inject = Clock::now();

        // Push event into ring buffer (producer → stream processor → broadcast)
        while (!ring.try_push(make_event(r)))
            std::this_thread::yield();

        // Wait until all connected clients have received the frame
        auto round_deadline = Clock::now() + std::chrono::seconds(5);
        while (recv_count[r].load(std::memory_order_acquire) < connected_count.load()
               && Clock::now() < round_deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        auto t_all_recv = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t_all_recv - t_inject).count();

        if (r >= WARMUP) {
            latencies_ms.push_back(ms);
            std::printf("  round %3d/%d  → %6.2f ms  (%d clients)\n",
                        r - WARMUP + 1, ROUNDS, ms,
                        recv_count[r].load());
        } else {
            std::printf("  warmup %d/%d  → %6.2f ms\n", r + 1, WARMUP, ms);
        }

        // Small gap between rounds so events don't pile up
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // ── Statistics ─────────────────────────────────────────────────────
    std::sort(latencies_ms.begin(), latencies_ms.end());
    double mean = std::accumulate(latencies_ms.begin(), latencies_ms.end(), 0.0)
                / latencies_ms.size();
    double p50  = latencies_ms[latencies_ms.size() * 50 / 100];
    double p95  = latencies_ms[latencies_ms.size() * 95 / 100];
    double p99  = latencies_ms[latencies_ms.size() * 99 / 100];
    double best = latencies_ms.front();
    double worst= latencies_ms.back();

    std::printf("\n──────────────────────────────────────────────────────\n");
    std::printf("  Broadcast latency (%d clients, %d rounds)\n", N_CLIENTS, ROUNDS);
    std::printf("  mean : %6.2f ms\n", mean);
    std::printf("  p50  : %6.2f ms\n", p50);
    std::printf("  p95  : %6.2f ms\n", p95);
    std::printf("  p99  : %6.2f ms\n", p99);
    std::printf("  best : %6.2f ms\n", best);
    std::printf("  worst: %6.2f ms\n", worst);
    std::printf("──────────────────────────────────────────────────────\n");

    bool pass = (p99 < 20.0);
    std::printf("  Target (p99 < 20ms): %s\n", pass ? "PASS ✓" : "FAIL ✗");
    std::printf("══════════════════════════════════════════════════════\n\n");

    // Shutdown
    server.stop();
    for (auto& t : client_threads)
        if (t.joinable()) t.join();
    server_thread.join();
    proc.stop();

    return pass ? 0 : 1;
}
