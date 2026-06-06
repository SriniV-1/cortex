#pragma once
// ConnectionPool — thread-safe pool of pqxx::connection objects.
//
// Connections are created lazily up to a configurable maximum.  When all
// connections are in use, acquire() blocks until one is returned.  The
// PooledConnection RAII guard ensures connections are always released back
// to the pool, even when an exception is thrown.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include <pqxx/pqxx>

namespace cortex::serving {

class ConnectionPool;

// ── RAII guard — auto-releases the connection on destruction ───────────────
class PooledConnection {
public:
    PooledConnection() = default;
    PooledConnection(pqxx::connection* conn, ConnectionPool* pool)
        : conn_(conn), pool_(pool) {}

    ~PooledConnection();

    // Move-only
    PooledConnection(PooledConnection&& o) noexcept
        : conn_(o.conn_), pool_(o.pool_) { o.conn_ = nullptr; o.pool_ = nullptr; }
    PooledConnection& operator=(PooledConnection&& o) noexcept {
        if (this != &o) { release(); conn_ = o.conn_; pool_ = o.pool_; o.conn_ = nullptr; o.pool_ = nullptr; }
        return *this;
    }
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    pqxx::connection* get() const noexcept { return conn_; }
    explicit operator bool() const noexcept { return conn_ != nullptr; }

private:
    void release();
    pqxx::connection* conn_ = nullptr;
    ConnectionPool*   pool_ = nullptr;
};

// ── Connection pool ───────────────────────────────────────────────────────
class ConnectionPool {
public:
    explicit ConnectionPool(std::string conn_str, size_t max_size = 4)
        : conn_str_(std::move(conn_str)), max_size_(max_size) {}

    // Acquire a connection from the pool (blocks if none available).
    PooledConnection acquire() {
        std::unique_lock lock(mu_);
        while (idle_.empty() && created_ >= max_size_)
            cv_.wait(lock);

        if (!idle_.empty()) {
            auto* conn = idle_.front();
            idle_.pop();
            lock.unlock();
            // Reconnect if the connection dropped
            if (!conn->is_open()) {
                try { conn->close(); delete conn; }
                catch (...) {}
                conn = new pqxx::connection(conn_str_);
            }
            return PooledConnection(conn, this);
        }

        // Create a new connection (still under max_size)
        ++created_;
        lock.unlock();
        auto* conn = new pqxx::connection(conn_str_);
        return PooledConnection(conn, this);
    }

    // Return a connection to the pool.
    void release(pqxx::connection* conn) {
        if (!conn) return;
        std::lock_guard lock(mu_);
        idle_.push(conn);
        cv_.notify_one();
    }

    ~ConnectionPool() {
        std::lock_guard lock(mu_);
        while (!idle_.empty()) {
            delete idle_.front();
            idle_.pop();
        }
    }

    // Non-copyable, non-movable
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

private:
    std::string                      conn_str_;
    size_t                           max_size_;
    size_t                           created_ = 0;
    std::mutex                       mu_;
    std::condition_variable          cv_;
    std::queue<pqxx::connection*>    idle_;
};

// ── PooledConnection out-of-line definitions ──────────────────────────────
inline PooledConnection::~PooledConnection() { release(); }
inline void PooledConnection::release() {
    if (conn_ && pool_) { pool_->release(conn_); conn_ = nullptr; pool_ = nullptr; }
}

} // namespace cortex::serving
