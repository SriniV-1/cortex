#pragma once
// IngestorNode — a worker in the distributed stream processing cluster.
//
// Lifecycle:
//   1. Generates a UUID worker_id on construction.
//   2. Registers with the Coordinator via gRPC.
//   3. Opens a StreamAssignments stream to receive game assignments.
//   4. Sends heartbeats every 2 seconds.
//   5. For each assigned game, spawns a poll thread that fetches play-by-play
//      from NBA S3 and pushes events into a shared RingBuffer.
//   6. On REVOKE or stop(), cleanly shuts down per-game poll threads.
//   7. On stop(), sends Deregister RPC for immediate game reassignment.
//
// Thread model:
//   - 1 heartbeat thread
//   - 1 assignment listener thread (gRPC streaming)
//   - N per-game poll threads (one per assigned game)
//   - 1 StreamProcessor consumer thread (from RingBuffer)
//   The RingBuffer is SPSC: all per-game threads push into a thread-safe
//   queue, and a single "feeder" thread drains that queue into the RingBuffer.

#include "etl/NBAClient.hpp"
#include "stream/RingBuffer.hpp"
#include "stream/StreamEvent.hpp"
#include "stream/StreamProcessor.hpp"
#include "stream/StatAccumulator.hpp"
#include "common/Logger.hpp"

#include <cortex.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

namespace cortex::distributed {

class IngestorNode {
public:
    struct Config {
        std::string coordinator_address;  // e.g., "coordinator:50051"
        std::string host;                 // this node's reachable host
        int32_t http_port = 0;
        int32_t capacity = 20;
        int heartbeat_interval_ms = 2000;
        int poll_interval_ms = 5000;
    };

    explicit IngestorNode(Config cfg);
    ~IngestorNode();

    void start();
    void stop();

    bool running() const noexcept { return running_.load(); }
    const std::string& worker_id() const noexcept { return worker_id_; }

    // Access local stats for HTTP serving on this node.
    cortex::stream::StatAccumulator& stats() { return accumulator_; }

private:
    void register_with_coordinator();
    void heartbeat_loop();
    void assignment_listener();
    void feeder_loop();

    void handle_assign(const std::string& game_id, int64_t epoch);
    void handle_revoke(const std::string& game_id);

    void poll_game(const std::string& game_id, int64_t epoch);

    // Convert PlayAction to StreamEvent (same logic as LiveIngestor).
    static cortex::stream::StreamEvent make_event(
        const cortex::etl::PlayAction& action,
        const std::string& game_id,
        int32_t home_team_id, int32_t away_team_id);
    static uint16_t parse_clock_secs(const std::string& clock) noexcept;

    Config config_;
    std::string worker_id_;

    // gRPC channel to coordinator.
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<cortex::CoordinatorService::Stub> stub_;

    // Per-game state.
    struct GameState {
        int64_t epoch;
        int64_t watermark = 0;       // highest order_number seen
        int32_t home_team_id = 0;
        int32_t away_team_id = 0;
        std::atomic<bool> active{true};
        std::thread poll_thread;
    };
    std::mutex games_mu_;
    std::unordered_map<std::string, std::unique_ptr<GameState>> active_games_;

    // Thread-safe event queue: per-game poll threads push here,
    // feeder thread drains into the SPSC RingBuffer.
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<cortex::stream::StreamEvent> event_queue_;

    // Data plane.
    cortex::stream::RingBuffer<cortex::stream::StreamEvent> ring_{65536};
    cortex::stream::StatAccumulator accumulator_;
    std::unique_ptr<cortex::stream::StreamProcessor> processor_;

    cortex::etl::NBAClient nba_client_{50};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::atomic<int64_t> total_events_{0};
    std::thread heartbeat_thread_;
    std::thread assignment_thread_;
    std::thread feeder_thread_;
};

} // namespace cortex::distributed
