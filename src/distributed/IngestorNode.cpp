#include "distributed/IngestorNode.hpp"
#include "stream/StreamEvent.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

namespace cortex::distributed {

using cortex::stream::StreamEvent;
using cortex::stream::ActionType;
using cortex::stream::parse_action_type;

// ── UUID v4 generator ────────────────────────────────────────────────────────

static std::string generate_uuid() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);

    // Set version 4 and variant bits.
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>((a >> 16) & 0xFFFF),
        static_cast<uint16_t>(a & 0xFFFF),
        static_cast<uint16_t>(b >> 48),
        static_cast<unsigned long long>(b & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

// ── Construction ─────────────────────────────────────────────────────────────

IngestorNode::IngestorNode(Config cfg)
    : config_(std::move(cfg))
    , worker_id_(generate_uuid())
{
    channel_ = grpc::CreateChannel(config_.coordinator_address,
                                   grpc::InsecureChannelCredentials());
    stub_ = cortex::CoordinatorService::NewStub(channel_);
}

IngestorNode::~IngestorNode() { stop(); }

// ── Lifecycle ────────────────────────────────────────────────────────────────

void IngestorNode::start() {
    if (running_.load()) return;
    auto log = cortex::get_logger("worker");

    stop_flag_.store(false);
    running_.store(true);

    // Start the stream processor (consumes from ring buffer).
    processor_ = std::make_unique<cortex::stream::StreamProcessor>(
        ring_, accumulator_);
    processor_->start([](const StreamEvent&) {
        // No broadcast callback on worker — coordinator aggregates stats.
    });

    // Register with coordinator.
    register_with_coordinator();

    // Start background threads.
    feeder_thread_ = std::thread([this] { feeder_loop(); });
    heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
    assignment_thread_ = std::thread([this] { assignment_listener(); });

    log->info("IngestorNode {} started (coordinator={})",
              worker_id_, config_.coordinator_address);
}

void IngestorNode::stop() {
    if (!running_.load()) return;
    auto log = cortex::get_logger("worker");
    log->info("IngestorNode {} shutting down…", worker_id_);

    stop_flag_.store(true);
    queue_cv_.notify_all();

    // Deregister from coordinator for immediate reassignment.
    try {
        cortex::DeregisterRequest req;
        req.set_worker_id(worker_id_);
        cortex::Ack resp;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        stub_->Deregister(&ctx, req, &resp);
    } catch (...) {}

    // Stop all per-game poll threads.
    {
        std::lock_guard lock(games_mu_);
        for (auto& [gid, state] : active_games_) {
            state->active.store(false);
        }
    }
    {
        std::lock_guard lock(games_mu_);
        for (auto& [gid, state] : active_games_) {
            if (state->poll_thread.joinable()) state->poll_thread.join();
        }
        active_games_.clear();
    }

    if (feeder_thread_.joinable()) feeder_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (assignment_thread_.joinable()) assignment_thread_.join();

    if (processor_) processor_->stop();

    running_.store(false);
    log->info("IngestorNode {} stopped. Total events: {}",
              worker_id_, total_events_.load());
}

// ── Registration ─────────────────────────────────────────────────────────────

void IngestorNode::register_with_coordinator() {
    auto log = cortex::get_logger("worker");

    cortex::RegisterRequest req;
    req.set_worker_id(worker_id_);
    req.set_host(config_.host);
    req.set_port(config_.http_port);
    req.set_capacity(config_.capacity);

    cortex::RegisterResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    auto status = stub_->Register(&ctx, req, &resp);
    if (!status.ok()) {
        log->error("Registration failed: {}", status.error_message());
        return;
    }

    if (resp.accepted()) {
        log->info("Registered with coordinator (epoch={})", resp.epoch());
    } else {
        log->error("Registration rejected by coordinator");
    }
}

// ── Heartbeat ────────────────────────────────────────────────────────────────

void IngestorNode::heartbeat_loop() {
    auto log = cortex::get_logger("worker");

    while (!stop_flag_.load()) {
        try {
            cortex::HeartbeatRequest req;
            req.set_worker_id(worker_id_);
            req.set_events_processed(total_events_.load());

            {
                std::lock_guard lock(games_mu_);
                for (const auto& [gid, _] : active_games_) {
                    req.add_active_games(gid);
                }
            }

            cortex::HeartbeatResponse resp;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now()
                             + std::chrono::seconds(2));
            stub_->Heartbeat(&ctx, req, &resp);

        } catch (const std::exception& e) {
            log->warn("Heartbeat failed: {}", e.what());
        }

        // Interruptible sleep.
        for (int i = 0; i < config_.heartbeat_interval_ms / 100
                     && !stop_flag_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ── Assignment listener ──────────────────────────────────────────────────────

void IngestorNode::assignment_listener() {
    auto log = cortex::get_logger("worker");

    while (!stop_flag_.load()) {
        try {
            cortex::AssignmentRequest req;
            req.set_worker_id(worker_id_);

            grpc::ClientContext ctx;
            auto reader = stub_->StreamAssignments(&ctx, req);

            cortex::AssignmentUpdate update;
            while (reader->Read(&update) && !stop_flag_.load()) {
                if (update.action() == cortex::AssignmentUpdate::ASSIGN) {
                    log->info("Assigned game {} (epoch={})",
                              update.game_id(), update.epoch());
                    handle_assign(update.game_id(), update.epoch());
                } else if (update.action() == cortex::AssignmentUpdate::REVOKE) {
                    log->info("Revoked game {} (epoch={})",
                              update.game_id(), update.epoch());
                    handle_revoke(update.game_id());
                }
            }

            auto status = reader->Finish();
            if (!status.ok() && !stop_flag_.load()) {
                log->warn("Assignment stream disconnected: {}. Reconnecting…",
                          status.error_message());
            }
        } catch (const std::exception& e) {
            log->warn("Assignment listener error: {}. Reconnecting…", e.what());
        }

        // Backoff before reconnecting.
        if (!stop_flag_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

// ── Feeder loop: drains thread-safe queue into SPSC ring buffer ─────────────

void IngestorNode::feeder_loop() {
    auto log = cortex::get_logger("worker");

    while (!stop_flag_.load()) {
        std::unique_lock lock(queue_mu_);
        queue_cv_.wait_for(lock, std::chrono::milliseconds(50),
                           [this] { return !event_queue_.empty() || stop_flag_.load(); });

        while (!event_queue_.empty()) {
            auto ev = std::move(event_queue_.front());
            event_queue_.pop();
            lock.unlock();

            // Push into SPSC ring buffer (single producer = this feeder thread).
            int retries = 0;
            while (!ring_.try_push(ev) && retries < 1000) {
                std::this_thread::yield();
                ++retries;
            }
            if (retries == 1000) {
                log->warn("Ring buffer full — dropped event");
            }

            lock.lock();
        }
    }
}

// ── Game management ──────────────────────────────────────────────────────────

void IngestorNode::handle_assign(const std::string& game_id, int64_t epoch) {
    std::lock_guard lock(games_mu_);

    // Reject if already tracking this game with a newer epoch.
    auto it = active_games_.find(game_id);
    if (it != active_games_.end()) {
        if (it->second->epoch >= epoch) return;
        // Stale epoch — stop old thread, replace.
        it->second->active.store(false);
        if (it->second->poll_thread.joinable()) it->second->poll_thread.detach();
        active_games_.erase(it);
    }

    auto state = std::make_unique<GameState>();
    state->epoch = epoch;

    // Fetch team IDs from scoreboard for this game.
    try {
        auto games = nba_client_.fetch_scoreboard();
        for (const auto& gs : games) {
            if (gs.game_id == game_id) {
                state->home_team_id = gs.home_team_id;
                state->away_team_id = gs.away_team_id;
                break;
            }
        }
    } catch (...) {}

    state->poll_thread = std::thread([this, game_id, epoch] {
        poll_game(game_id, epoch);
    });

    active_games_[game_id] = std::move(state);
}

void IngestorNode::handle_revoke(const std::string& game_id) {
    std::lock_guard lock(games_mu_);
    auto it = active_games_.find(game_id);
    if (it == active_games_.end()) return;

    it->second->active.store(false);
    if (it->second->poll_thread.joinable()) it->second->poll_thread.join();
    active_games_.erase(it);
}

// ── Per-game poll thread ─────────────────────────────────────────────────────

void IngestorNode::poll_game(const std::string& game_id, int64_t epoch) {
    auto log = cortex::get_logger("worker");

    int32_t home_id = 0, away_id = 0;
    {
        std::lock_guard lock(games_mu_);
        auto it = active_games_.find(game_id);
        if (it != active_games_.end()) {
            home_id = it->second->home_team_id;
            away_id = it->second->away_team_id;
        }
    }

    int64_t watermark = 0;

    while (!stop_flag_.load()) {
        // Check if still active.
        {
            std::lock_guard lock(games_mu_);
            auto it = active_games_.find(game_id);
            if (it == active_games_.end() || !it->second->active.load()) break;
        }

        try {
            auto pbp = nba_client_.fetch_play_by_play(game_id);
            if (!pbp) {
                // Game may not exist yet or S3 returned error.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.poll_interval_ms));
                continue;
            }

            int pushed = 0;
            for (const auto& action : pbp->actions) {
                if (action.order_number <= watermark) continue;

                auto ev = make_event(action, game_id, home_id, away_id);

                {
                    std::lock_guard lock(queue_mu_);
                    event_queue_.push(ev);
                }
                queue_cv_.notify_one();

                watermark = std::max(watermark, action.order_number);
                ++pushed;
                total_events_.fetch_add(1);
            }

            if (pushed > 0) {
                log->debug("Game {}: pushed {} new events (watermark={})",
                           game_id, pushed, watermark);
            }

            // Update watermark in game state.
            {
                std::lock_guard lock(games_mu_);
                auto it = active_games_.find(game_id);
                if (it != active_games_.end()) {
                    it->second->watermark = watermark;
                }
            }

        } catch (const std::exception& e) {
            log->warn("Poll game {} error: {}", game_id, e.what());
        }

        // Interruptible sleep.
        for (int i = 0; i < config_.poll_interval_ms / 100
                     && !stop_flag_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ── Event conversion (mirrors LiveIngestor logic) ────────────────────────────

uint16_t IngestorNode::parse_clock_secs(const std::string& clock) noexcept {
    if (clock.size() < 3 || clock[0] != 'P' || clock[1] != 'T') return 0;

    int minutes = 0, seconds = 0;
    const char* p = clock.c_str() + 2;
    char* end = nullptr;

    long m = std::strtol(p, &end, 10);
    if (end && *end == 'M') {
        minutes = static_cast<int>(m);
        p = end + 1;
        long s = std::strtol(p, &end, 10);
        seconds = static_cast<int>(s);
    } else {
        long s = std::strtol(p, &end, 10);
        seconds = static_cast<int>(s);
    }

    uint32_t total = static_cast<uint32_t>(minutes * 60 + seconds);
    return static_cast<uint16_t>(std::min(total, static_cast<uint32_t>(65535)));
}

StreamEvent IngestorNode::make_event(
        const cortex::etl::PlayAction& action,
        const std::string& game_id,
        int32_t home_team_id,
        int32_t away_team_id) {

    StreamEvent ev{};

    ev.event_id = action.action_number;
    ev.order_number = action.order_number;

    const char* gid = game_id.c_str();
    std::size_t glen = std::min(game_id.size(), ev.game_id.size() - 1);
    std::copy(gid, gid + glen, ev.game_id.begin());
    ev.game_id[glen] = '\0';

    ev.home_team_id = home_team_id;
    ev.away_team_id = away_team_id;

    ev.player_id = action.person_id;
    ev.team_id = action.team_id;
    ev.action_type = parse_action_type(action.action_type);
    ev.period = static_cast<uint8_t>(std::clamp(action.period, 0, 255));
    ev.score_home = action.score_home;
    ev.score_away = action.score_away;
    ev.x = action.x;
    ev.y = action.y;
    ev.clock_secs = parse_clock_secs(action.clock);

    if (ev.action_type == ActionType::Shot2pt
     || ev.action_type == ActionType::Shot3pt
     || ev.action_type == ActionType::FreeThrow) {
        ev.shot_made = !action.description.empty()
                    && action.description.compare(0, 4, "MISS") != 0;
    }

    return ev;
}

} // namespace cortex::distributed
