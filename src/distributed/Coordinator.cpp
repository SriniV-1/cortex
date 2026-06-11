#include "distributed/Coordinator.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace cortex::distributed {

Coordinator::Coordinator() : config_{} {}

Coordinator::Coordinator(Config cfg)
    : config_(std::move(cfg)) {}

Coordinator::~Coordinator() { stop(); }

// ── Lifecycle ────────────────────────────────────────────────────────────────

void Coordinator::start() {
    if (running_.load()) return;
    auto log = cortex::get_logger("coordinator");

    // Start gRPC server.
    grpc::ServerBuilder builder;
    builder.AddListeningPort(config_.grpc_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    grpc_server_ = builder.BuildAndStart();
    if (!grpc_server_) {
        log->error("Failed to start gRPC server on {}", config_.grpc_address);
        return;
    }
    log->info("Coordinator gRPC server listening on {}", config_.grpc_address);

    stop_flag_.store(false);
    running_.store(true);

    // Background threads for game assignment and failure detection.
    assignment_thread_ = std::thread([this] { assignment_loop(); });
    failure_thread_ = std::thread([this] { failure_detection_loop(); });
}

void Coordinator::stop() {
    if (!running_.load()) return;
    auto log = cortex::get_logger("coordinator");
    log->info("Coordinator shutting down…");

    stop_flag_.store(true);
    cv_.notify_all();

    if (grpc_server_) grpc_server_->Shutdown();
    if (assignment_thread_.joinable()) assignment_thread_.join();
    if (failure_thread_.joinable()) failure_thread_.join();

    running_.store(false);
    log->info("Coordinator stopped.");
}

// ── gRPC: Register ───────────────────────────────────────────────────────────

grpc::Status Coordinator::Register(
        grpc::ServerContext* /*ctx*/,
        const cortex::RegisterRequest* req,
        cortex::RegisterResponse* resp) {

    auto log = cortex::get_logger("coordinator");
    std::lock_guard lock(mu_);

    if (workers_.count(req->worker_id())) {
        log->warn("Duplicate registration from {}", req->worker_id());
        resp->set_accepted(false);
        resp->set_coordinator_id("cortex-coordinator");
        return grpc::Status::OK;
    }

    WorkerInfo info;
    info.worker_id = req->worker_id();
    info.host = req->host();
    info.port = req->port();
    info.capacity = req->capacity();
    info.last_heartbeat = std::chrono::steady_clock::now();
    info.epoch = global_epoch_.fetch_add(1);
    info.health = WorkerHealth::Healthy;

    workers_[info.worker_id] = std::move(info);

    // Add to hash ring and compute migrations.
    auto migrated = hash_ring_.add_node(req->worker_id());
    log->info("Worker {} registered (host={}:{}, capacity={}). {} games migrated.",
              req->worker_id(), req->host(), req->port(), req->capacity(),
              migrated.size());

    // Reassign migrated games.
    for (const auto& gid : migrated) {
        auto prev_it = game_owners_.find(gid);
        if (prev_it != game_owners_.end() && prev_it->second != req->worker_id()) {
            // Revoke from previous owner.
            auto old_worker_it = workers_.find(prev_it->second);
            if (old_worker_it != workers_.end()) {
                revoke_game(gid, prev_it->second);
            }
        }
        // Assign to new node.
        game_owners_[gid] = req->worker_id();
        auto& w = workers_[req->worker_id()];
        w.assigned_games.push_back(gid);
    }

    resp->set_accepted(true);
    resp->set_epoch(info.epoch);
    resp->set_coordinator_id("cortex-coordinator");

    return grpc::Status::OK;
}

// ── gRPC: Heartbeat ──────────────────────────────────────────────────────────

grpc::Status Coordinator::Heartbeat(
        grpc::ServerContext* /*ctx*/,
        const cortex::HeartbeatRequest* req,
        cortex::HeartbeatResponse* resp) {

    std::lock_guard lock(mu_);
    auto it = workers_.find(req->worker_id());
    if (it == workers_.end()) {
        resp->set_acknowledged(false);
        return grpc::Status::OK;
    }

    auto& w = it->second;
    w.last_heartbeat = std::chrono::steady_clock::now();
    w.events_processed = req->events_processed();
    w.cpu_load = req->cpu_load();

    // Recover from degraded state on heartbeat.
    if (w.health == WorkerHealth::Degraded) {
        w.health = WorkerHealth::Healthy;
        auto log = cortex::get_logger("coordinator");
        log->info("Worker {} recovered (Degraded -> Healthy)", w.worker_id);
    }

    resp->set_acknowledged(true);
    return grpc::Status::OK;
}

// ── gRPC: StreamAssignments ──────────────────────────────────────────────────

grpc::Status Coordinator::StreamAssignments(
        grpc::ServerContext* ctx,
        const cortex::AssignmentRequest* req,
        grpc::ServerWriter<cortex::AssignmentUpdate>* writer) {

    auto log = cortex::get_logger("coordinator");

    // Store the writer so other threads can push assignments.
    {
        std::lock_guard lock(mu_);
        auto it = workers_.find(req->worker_id());
        if (it == workers_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Unknown worker");
        }
        it->second.stream = writer;

        // Send current assignments immediately.
        for (const auto& gid : it->second.assigned_games) {
            cortex::AssignmentUpdate update;
            update.set_action(cortex::AssignmentUpdate::ASSIGN);
            update.set_game_id(gid);
            update.set_epoch(it->second.epoch);
            writer->Write(update);
        }
    }

    log->info("Worker {} connected to assignment stream", req->worker_id());

    // Block until client disconnects or server shuts down.
    while (!ctx->IsCancelled() && !stop_flag_.load()) {
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, std::chrono::seconds(1));
    }

    // Clear the stream pointer.
    {
        std::lock_guard lock(mu_);
        auto it = workers_.find(req->worker_id());
        if (it != workers_.end()) {
            it->second.stream = nullptr;
        }
    }

    return grpc::Status::OK;
}

// ── gRPC: ReportStats ────────────────────────────────────────────────────────

grpc::Status Coordinator::ReportStats(
        grpc::ServerContext* /*ctx*/,
        const cortex::StatsReport* req,
        cortex::Ack* resp) {

    std::lock_guard lock(mu_);
    auto it = workers_.find(req->worker_id());
    if (it != workers_.end()) {
        it->second.events_processed = req->events_processed();
    }
    resp->set_ok(true);
    return grpc::Status::OK;
}

// ── gRPC: Deregister ─────────────────────────────────────────────────────────

grpc::Status Coordinator::Deregister(
        grpc::ServerContext* /*ctx*/,
        const cortex::DeregisterRequest* req,
        cortex::Ack* resp) {

    auto log = cortex::get_logger("coordinator");
    log->info("Worker {} deregistering (graceful shutdown)", req->worker_id());

    std::lock_guard lock(mu_);
    reassign_dead_worker(req->worker_id());

    resp->set_ok(true);
    resp->set_message("Deregistered");
    return grpc::Status::OK;
}

// ── Assignment loop ──────────────────────────────────────────────────────────

void Coordinator::assignment_loop() {
    auto log = cortex::get_logger("coordinator");
    log->info("Assignment loop started (interval: {}ms)", config_.assignment_interval_ms);

    while (!stop_flag_.load()) {
        try {
            auto games = nba_client_.fetch_scoreboard();

            std::lock_guard lock(mu_);

            // Track new live games, untrack finished ones.
            for (const auto& gs : games) {
                if (gs.status == 2) {
                    // Live game — assign if not already tracked.
                    if (!game_owners_.count(gs.game_id)) {
                        hash_ring_.track_game(gs.game_id);
                        assign_game(gs.game_id);
                    }
                } else if (gs.status == 3) {
                    // Finished game — revoke assignment.
                    auto oit = game_owners_.find(gs.game_id);
                    if (oit != game_owners_.end()) {
                        log->info("Game {} finished — revoking from {}",
                                  gs.game_id, oit->second);
                        revoke_game(gs.game_id, oit->second);
                        hash_ring_.untrack_game(gs.game_id);
                        game_owners_.erase(oit);
                    }
                }
            }

        } catch (const std::exception& e) {
            log->warn("Assignment loop error: {}", e.what());
        }

        // Interruptible sleep.
        for (int i = 0; i < config_.assignment_interval_ms / 100
                     && !stop_flag_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    log->info("Assignment loop stopped.");
}

// ── Failure detection ────────────────────────────────────────────────────────

void Coordinator::failure_detection_loop() {
    auto log = cortex::get_logger("coordinator");
    log->info("Failure detection started (timeout: {}ms)", config_.heartbeat_timeout_ms);

    while (!stop_flag_.load()) {
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard lock(mu_);
            std::vector<std::string> dead_workers;

            for (auto& [id, w] : workers_) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - w.last_heartbeat).count();

                if (w.health == WorkerHealth::Healthy
                    && elapsed > config_.heartbeat_timeout_ms / 2) {
                    w.health = WorkerHealth::Degraded;
                    log->warn("Worker {} degraded (no heartbeat for {}ms)", id, elapsed);
                }

                if (elapsed > config_.heartbeat_timeout_ms) {
                    w.health = WorkerHealth::Dead;
                    dead_workers.push_back(id);
                    log->error("Worker {} DEAD (no heartbeat for {}ms)", id, elapsed);
                }
            }

            for (const auto& id : dead_workers) {
                reassign_dead_worker(id);
            }
        }

        // Interruptible sleep.
        for (int i = 0; i < config_.failure_check_interval_ms / 100
                     && !stop_flag_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    log->info("Failure detection stopped.");
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void Coordinator::assign_game(const std::string& game_id) {
    auto log = cortex::get_logger("coordinator");
    auto owner = hash_ring_.assign(game_id);
    if (!owner) {
        log->warn("No workers available to assign game {}", game_id);
        return;
    }

    auto it = workers_.find(*owner);
    if (it == workers_.end()) return;

    int64_t epoch = global_epoch_.fetch_add(1);
    game_owners_[game_id] = *owner;
    it->second.assigned_games.push_back(game_id);

    send_assignment(it->second, game_id,
                    cortex::AssignmentUpdate::ASSIGN, epoch);
    log->info("Assigned game {} to worker {} (epoch={})",
              game_id, *owner, epoch);
}

void Coordinator::revoke_game(const std::string& game_id,
                               const std::string& worker_id) {
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;

    auto& games = it->second.assigned_games;
    games.erase(std::remove(games.begin(), games.end(), game_id), games.end());

    int64_t epoch = global_epoch_.fetch_add(1);
    send_assignment(it->second, game_id,
                    cortex::AssignmentUpdate::REVOKE, epoch);
}

void Coordinator::reassign_dead_worker(const std::string& worker_id) {
    auto log = cortex::get_logger("coordinator");

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;

    auto dead_games = it->second.assigned_games;
    workers_.erase(it);

    // Remove from hash ring — returns new assignments.
    auto reassignments = hash_ring_.remove_node(worker_id);

    // Clear game_owners for this worker's games.
    for (const auto& gid : dead_games) {
        game_owners_.erase(gid);
    }

    // Reassign via hash ring results.
    int reassigned = 0;
    for (const auto& [gid, new_owner] : reassignments) {
        auto wit = workers_.find(new_owner);
        if (wit == workers_.end()) continue;

        int64_t epoch = global_epoch_.fetch_add(1);
        game_owners_[gid] = new_owner;
        wit->second.assigned_games.push_back(gid);
        send_assignment(wit->second, gid,
                        cortex::AssignmentUpdate::ASSIGN, epoch);
        ++reassigned;
    }

    log->info("Worker {} removed. {} games reassigned to surviving workers.",
              worker_id, reassigned);
}

void Coordinator::send_assignment(WorkerInfo& worker,
                                   const std::string& game_id,
                                   cortex::AssignmentUpdate::Action action,
                                   int64_t epoch) {
    if (!worker.stream) return;

    cortex::AssignmentUpdate update;
    update.set_action(action);
    update.set_game_id(game_id);
    update.set_epoch(epoch);

    if (!worker.stream->Write(update)) {
        auto log = cortex::get_logger("coordinator");
        log->warn("Failed to write assignment to worker {}", worker.worker_id);
        worker.stream = nullptr;
    }

    cv_.notify_all();
}

// ── Cluster status ───────────────────────────────────────────────────────────

Coordinator::ClusterStatus Coordinator::cluster_status() const {
    std::lock_guard lock(mu_);
    ClusterStatus status;
    status.total_workers = static_cast<int>(workers_.size());
    status.total_games = static_cast<int>(game_owners_.size());

    for (const auto& [id, w] : workers_) {
        status.workers.push_back({
            id, w.health,
            static_cast<int>(w.assigned_games.size()),
            w.events_processed
        });
    }
    return status;
}

} // namespace cortex::distributed
