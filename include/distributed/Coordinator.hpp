#pragma once
// Coordinator — single leader that manages the distributed stream processing
// cluster. Tracks worker membership via heartbeat, assigns live NBA games to
// workers via consistent hashing, and handles failure recovery.
//
// Communication: gRPC server on configurable port (default 50051).
// Workers Register → receive streaming AssignmentUpdates → send Heartbeats.
// If a worker misses 3 heartbeats (6s), it's marked dead and its games are
// reassigned to surviving workers.

#include "distributed/ConsistentHashRing.hpp"
#include "etl/NBAClient.hpp"
#include "common/Logger.hpp"

#include <cortex.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cortex::distributed {

enum class WorkerHealth { Healthy, Degraded, Dead };

struct WorkerInfo {
    std::string worker_id;
    std::string host;
    int32_t     port = 0;
    int32_t     capacity = 0;
    std::chrono::steady_clock::time_point last_heartbeat;
    std::vector<std::string> assigned_games;
    int64_t     epoch = 0;
    int64_t     events_processed = 0;
    double      cpu_load = 0.0;
    WorkerHealth health = WorkerHealth::Healthy;

    // Server-streaming writer for pushing assignments. Null if stream dropped.
    grpc::ServerWriter<cortex::AssignmentUpdate>* stream = nullptr;
};

class Coordinator final : public cortex::CoordinatorService::Service {
public:
    struct Config {
        std::string grpc_address = "0.0.0.0:50051";
        int heartbeat_timeout_ms = 6000;       // 3 missed 2s heartbeats
        int assignment_interval_ms = 5000;     // scoreboard poll interval
        int failure_check_interval_ms = 2000;
    };

    Coordinator();
    explicit Coordinator(Config cfg);
    ~Coordinator() override;

    void start();
    void stop();
    bool running() const noexcept { return running_.load(); }

    // ── gRPC service methods ─────────────────────────────────────────────
    grpc::Status Register(grpc::ServerContext* ctx,
                          const cortex::RegisterRequest* req,
                          cortex::RegisterResponse* resp) override;

    grpc::Status Heartbeat(grpc::ServerContext* ctx,
                           const cortex::HeartbeatRequest* req,
                           cortex::HeartbeatResponse* resp) override;

    grpc::Status StreamAssignments(grpc::ServerContext* ctx,
                                   const cortex::AssignmentRequest* req,
                                   grpc::ServerWriter<cortex::AssignmentUpdate>* writer) override;

    grpc::Status ReportStats(grpc::ServerContext* ctx,
                             const cortex::StatsReport* req,
                             cortex::Ack* resp) override;

    grpc::Status Deregister(grpc::ServerContext* ctx,
                            const cortex::DeregisterRequest* req,
                            cortex::Ack* resp) override;

    // ── Cluster status for HTTP API ──────────────────────────────────────
    struct WorkerSnapshot {
        std::string  id;
        WorkerHealth health;
        int          games;
        int64_t      events_processed;
    };
    struct ClusterStatus {
        std::vector<WorkerSnapshot> workers;
        int total_games = 0;
        int total_workers = 0;
    };
    ClusterStatus cluster_status() const;

private:
    void assignment_loop();
    void failure_detection_loop();

    void assign_game(const std::string& game_id);
    void revoke_game(const std::string& game_id, const std::string& worker_id);
    void reassign_dead_worker(const std::string& worker_id);
    void send_assignment(WorkerInfo& worker, const std::string& game_id,
                         cortex::AssignmentUpdate::Action action, int64_t epoch);

    Config config_;
    ConsistentHashRing hash_ring_;
    cortex::etl::NBAClient nba_client_{200};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, WorkerInfo> workers_;
    std::unordered_map<std::string, std::string> game_owners_;  // game_id -> worker_id
    std::atomic<int64_t> global_epoch_{0};

    std::unique_ptr<grpc::Server> grpc_server_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::thread assignment_thread_;
    std::thread failure_thread_;
};

} // namespace cortex::distributed
