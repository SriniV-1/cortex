#include "serving/HttpServer.hpp"
#include "serving/KqueuePoller.hpp"
#include "serving/RedisCache.hpp"
#include "common/Logger.hpp"

#include <llhttp.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Base64 encode (minimal, for WebSocket handshake)
#include <openssl/evp.h>

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

// RFC 4648 base64 encode
static std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.resize(((len + 2) / 3) * 4 + 1);
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    out.resize(n);
    return out;
}

// WebSocket accept key: base64(SHA1(key + magic))
static std::string ws_accept_key(const std::string& key) {
    static constexpr char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;
    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.data()),
         combined.size(), sha);
    return base64_encode(sha, SHA_DIGEST_LENGTH);
}

// Extract a header value (case-insensitive field name)
static std::string get_header(const std::string& headers_raw,
                               const std::string& field) {
    std::string lower_field = field;
    std::transform(lower_field.begin(), lower_field.end(),
                   lower_field.begin(), ::tolower);

    std::istringstream ss(headers_raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line == "\r") continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string f = line.substr(0, colon);
        std::transform(f.begin(), f.end(), f.begin(), ::tolower);
        if (f == lower_field) {
            std::string v = line.substr(colon + 1);
            // trim leading/trailing whitespace and \r
            size_t s = v.find_first_not_of(" \t\r\n");
            size_t e = v.find_last_not_of(" \t\r\n");
            return (s == std::string::npos) ? "" : v.substr(s, e - s + 1);
        }
    }
    return "";
}

// JSON helpers
static std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    out += "\"";
    return out;
}

// ── llhttp callbacks (static — bound to Connection* via parser->data) ──────

static Connection* conn_from(llhttp_t* p) {
    return static_cast<Connection*>(p->data);
}

static int on_url(llhttp_t* p, const char* at, size_t len) {
    conn_from(p)->req_url_.append(at, len);
    return 0;
}
static int on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* c = conn_from(p);
    if (!c->cur_header_value_.empty()) {
        c->req_headers_raw_ += c->cur_header_field_ + ": "
                             + c->cur_header_value_ + "\n";
        c->cur_header_field_.clear();
        c->cur_header_value_.clear();
    }
    c->cur_header_field_.append(at, len);
    return 0;
}
static int on_header_value(llhttp_t* p, const char* at, size_t len) {
    conn_from(p)->cur_header_value_.append(at, len);
    return 0;
}
static int on_headers_complete(llhttp_t* p) {
    auto* c = conn_from(p);
    // flush last header
    if (!c->cur_header_field_.empty())
        c->req_headers_raw_ += c->cur_header_field_ + ": "
                             + c->cur_header_value_ + "\n";
    c->cur_header_field_.clear();
    c->cur_header_value_.clear();
    c->headers_complete_ = true;
    return 0;
}
static int on_body(llhttp_t* p, const char* at, size_t len) {
    conn_from(p)->req_body_.append(at, len);
    return 0;
}
static int on_message_complete(llhttp_t* p) {
    auto* c = conn_from(p);
    const char* method_str = llhttp_method_name(
        static_cast<llhttp_method_t>(p->method));
    c->process_http_request(method_str, c->req_url_,
                             c->req_headers_raw_, c->req_body_);
    // Reset for keep-alive
    c->req_url_.clear();
    c->req_headers_raw_.clear();
    c->req_body_.clear();
    c->headers_complete_ = false;
    return 0;
}

// ── Connection ─────────────────────────────────────────────────────────────

Connection::Connection(int fd, IOPoller& poller)
    : fd_(fd), poller_(poller)
{
    parser_          = std::make_unique<llhttp_t>();
    parser_settings_ = std::make_unique<llhttp_settings_t>();

    llhttp_settings_init(parser_settings_.get());
    parser_settings_->on_url              = on_url;
    parser_settings_->on_header_field     = on_header_field;
    parser_settings_->on_header_value     = on_header_value;
    parser_settings_->on_headers_complete = on_headers_complete;
    parser_settings_->on_body             = on_body;
    parser_settings_->on_message_complete = on_message_complete;

    llhttp_init(parser_.get(), HTTP_REQUEST, parser_settings_.get());
    parser_->data = this;
}

Connection::~Connection() {
    if (fd_ >= 0) {
        poller_.remove(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

void Connection::close() {
    state_ = State::Closing;
}

void Connection::on_readable() {
    char buf[16384];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            if (state_ == State::WebSocket) {
                read_buf_.append(buf, n);
                ws_parse_incoming();
            } else {
                enum llhttp_errno err = llhttp_execute(
                    parser_.get(), buf, static_cast<size_t>(n));
                if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
                    close();
                    return;
                }
            }
        } else if (n == 0) {
            close();
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return;
        }
    }
}

void Connection::on_writable() {
    if (state_ == State::WebSocket) {
        ws_flush_outbound();
        return;
    }
    // Flush HTTP write buffer
    while (!write_buf_.empty()) {
        ssize_t n = ::write(fd_, write_buf_.data(), write_buf_.size());
        if (n > 0) {
            write_buf_.erase(0, static_cast<size_t>(n));
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return;
        }
    }
    if (write_buf_.empty()) {
        // Close if flagged after flushing non-keepalive response
        if (close_after_flush_) {
            close();
            return;
        }
        // Otherwise remove write interest (re-add when more data queued)
        poller_.modify(fd_, IOEvent::Readable);
    }
}

void Connection::send_response(int status, const std::string& ct,
                                const std::string& body, bool keep_alive) {
    std::string reason;
    switch (status) {
    case 200: reason = "OK";          break;
    case 400: reason = "Bad Request"; break;
    case 404: reason = "Not Found";   break;
    case 500: reason = "Server Error";break;
    default:  reason = "Unknown";     break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << reason << "\r\n"
        << "Content-Type: "   << ct        << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: "     << (keep_alive ? "keep-alive" : "close") << "\r\n"
        << "\r\n"
        << body;

    write_buf_ += oss.str();
    if (!keep_alive) close_after_flush_ = true;
    poller_.modify(fd_, IOEvent::Readable | IOEvent::Writable);
}

// ── HTTP request handler ─────────────────────────────────────────────────

void Connection::process_http_request(const std::string& method,
                                       const std::string& path,
                                       const std::string& headers_raw,
                                       const std::string& body) {
    auto log = cortex::get_logger("http");
    log->debug("{} {}", method, path);

    // ── /health ──────────────────────────────────────────────────────────
    if (method == "GET" && path == "/health") {
        send_response(200, "application/json",
                      R"({"status":"ok","service":"cortex"})", false);
        return;
    }

    // ── /metrics (Prometheus) ────────────────────────────────────────────
    if (method == "GET" && path == "/metrics") {
        std::ostringstream m;
        m << "# HELP cortex_events_processed Total stream events processed\n"
          << "# TYPE cortex_events_processed counter\n";
        if (accumulator)
            m << "cortex_events_processed " << accumulator->event_count() << "\n";
        send_response(200, "text/plain; version=0.0.4", m.str(), false);
        return;
    }

    // ── /stats/{gameId} ──────────────────────────────────────────────────
    if (method == "GET" && path.rfind("/stats/", 0) == 0) {
        std::string game_id = path.substr(7);
        if (game_id.empty()) { send_response(400, "application/json",
                                              R"({"error":"missing game_id"})", false); return; }

        if (!accumulator) { send_response(500, "application/json",
                                           R"({"error":"no accumulator"})", false); return; }

        auto [home, away] = accumulator->score(game_id);
        std::ostringstream j;
        j << "{\"game_id\":" << json_str(game_id)
          << ",\"score_home\":" << home
          << ",\"score_away\":" << away
          << ",\"events_processed\":" << accumulator->event_count()
          << "}";
        send_response(200, "application/json", j.str(), false);
        return;
    }

    // ── /players/{playerId}/season ────────────────────────────────────────
    if (method == "GET" && path.rfind("/players/", 0) == 0) {
        // /players/{id}/season
        auto rest = path.substr(9);
        auto slash = rest.find('/');
        if (slash == std::string::npos || rest.substr(slash) != "/season") {
            send_response(404, "application/json", R"({"error":"not found"})", false);
            return;
        }
        std::string pid_str = rest.substr(0, slash);
        int32_t player_id = 0;
        try { player_id = std::stoi(pid_str); }
        catch (...) {
            send_response(400, "application/json", R"({"error":"invalid player_id"})", false);
            return;
        }

        if (!db_conn) {
            send_response(500, "application/json", R"({"error":"no db"})", false);
            return;
        }

        try {
            pqxx::work txn(*db_conn);
            auto r = txn.exec(
                "SELECT SUM(points) AS pts, SUM(rebounds) AS reb, "
                "       SUM(assists) AS ast, SUM(steals) AS stl, "
                "       SUM(blocks) AS blk, SUM(turnovers) AS to_, "
                "       SUM(fga) AS fga, SUM(fgm) AS fgm, "
                "       SUM(fta) AS fta, SUM(ftm) AS ftm, "
                "       COUNT(DISTINCT game_id) AS games "
                "FROM player_game_stats "
                "WHERE player_id = $1",
                pqxx::params{player_id});
            txn.commit();

            if (r.empty() || r[0][0].is_null()) {
                send_response(404, "application/json",
                              R"({"error":"player not found"})", false);
                return;
            }
            auto row = r[0];
            std::ostringstream j;
            j << "{\"player_id\":" << player_id
              << ",\"games\":"     << row["games"].as<int>(0)
              << ",\"points\":"    << row["pts"].as<int>(0)
              << ",\"rebounds\":"  << row["reb"].as<int>(0)
              << ",\"assists\":"   << row["ast"].as<int>(0)
              << ",\"steals\":"    << row["stl"].as<int>(0)
              << ",\"blocks\":"    << row["blk"].as<int>(0)
              << ",\"turnovers\":" << row["to_"].as<int>(0)
              << ",\"fga\":"       << row["fga"].as<int>(0)
              << ",\"fgm\":"       << row["fgm"].as<int>(0)
              << ",\"fta\":"       << row["fta"].as<int>(0)
              << ",\"ftm\":"       << row["ftm"].as<int>(0)
              << "}";
            send_response(200, "application/json", j.str(), false);
        } catch (const std::exception& e) {
            log->error("DB error: {}", e.what());
            send_response(500, "application/json", R"({"error":"db error"})", false);
        }
        return;
    }

    // ── /live/{gameId} — WebSocket upgrade ───────────────────────────────
    if (method == "GET" && path.rfind("/live/", 0) == 0) {
        std::string game_id = path.substr(6);

        std::string upgrade = get_header(headers_raw, "Upgrade");
        std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
        if (upgrade != "websocket") {
            send_response(400, "application/json",
                          R"({"error":"WebSocket upgrade required"})", false);
            return;
        }
        std::string ws_key = get_header(headers_raw, "Sec-WebSocket-Key");
        if (ws_key.empty()) {
            send_response(400, "application/json",
                          R"({"error":"missing Sec-WebSocket-Key"})", false);
            return;
        }
        upgrade_to_websocket(ws_key, game_id);
        return;
    }

    send_response(404, "application/json", R"({"error":"not found"})", false);
}

// ── WebSocket ─────────────────────────────────────────────────────────────

void Connection::upgrade_to_websocket(const std::string& key,
                                       const std::string& game_id) {
    std::string accept = ws_accept_key(key);
    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept << "\r\n"
         << "\r\n";

    write_buf_ += resp.str();
    state_ = State::WebSocket;
    subscribed_game_ = game_id;
    poller_.modify(fd_, IOEvent::Readable | IOEvent::Writable);

    auto log = cortex::get_logger("ws");
    log->info("WebSocket upgraded fd={} game={}", fd_, game_id);
}

// Encode a single text frame (unmasked — server→client)
std::string Connection::ws_encode_frame(const std::string& payload) {
    std::string frame;
    frame += static_cast<char>(0x81);  // FIN + opcode text

    size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(len);
    } else if (len < 65536) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len        & 0xFF);
    } else {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; --i)
            frame += static_cast<char>((len >> (8 * i)) & 0xFF);
    }
    frame += payload;
    return frame;
}

void Connection::ws_send(std::string payload) {
    std::string frame = ws_encode_frame(payload);
    {
        std::lock_guard lock(ws_out_mu_);
        ws_out_queue_.push(std::move(frame));
    }
    poller_.wakeup();  // wake event loop to flush
}

void Connection::ws_flush_outbound() {
    // Drain ws_out_queue_ → write_buf_ → socket
    {
        std::lock_guard lock(ws_out_mu_);
        while (!ws_out_queue_.empty()) {
            write_buf_ += ws_out_queue_.front();
            ws_out_queue_.pop();
        }
    }

    while (!write_buf_.empty()) {
        ssize_t n = ::write(fd_, write_buf_.data(), write_buf_.size());
        if (n > 0) {
            write_buf_.erase(0, static_cast<size_t>(n));
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return;
        } else {
            close();
            return;
        }
    }

    if (write_buf_.empty())
        poller_.modify(fd_, IOEvent::Readable);
}

// Minimal incoming frame parser: read and discard client→server frames
// (ping → pong; close → close; text ignored for now)
void Connection::ws_parse_incoming() {
    while (read_buf_.size() >= 2) {
        uint8_t b0 = static_cast<uint8_t>(read_buf_[0]);
        uint8_t b1 = static_cast<uint8_t>(read_buf_[1]);
        bool    masked     = (b1 & 0x80) != 0;
        uint8_t opcode     = b0 & 0x0F;
        size_t  payload_len = b1 & 0x7F;
        size_t  header_len  = 2 + (masked ? 4 : 0);

        if (payload_len == 126) {
            if (read_buf_.size() < 4) break;
            payload_len  = (static_cast<uint8_t>(read_buf_[2]) << 8)
                         | static_cast<uint8_t>(read_buf_[3]);
            header_len  += 2;
        } else if (payload_len == 127) {
            if (read_buf_.size() < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8)
                            | static_cast<uint8_t>(read_buf_[2 + i]);
            header_len += 8;
        }

        if (read_buf_.size() < header_len + payload_len) break;

        // Unmask if needed
        std::string payload(read_buf_.data() + header_len, payload_len);
        if (masked) {
            const char* mask = read_buf_.data() + header_len - 4;
            for (size_t i = 0; i < payload_len; ++i)
                payload[i] ^= mask[i % 4];
        }

        read_buf_.erase(0, header_len + payload_len);

        if (opcode == 0x8) {
            // Close frame — echo and close
            ws_send(std::string("\x03\xE8", 2));  // 1000 Normal Closure
            close();
            return;
        }
        if (opcode == 0x9) {
            // Ping — send pong
            std::string pong_frame;
            pong_frame += static_cast<char>(0x8A);  // FIN + pong
            pong_frame += static_cast<char>(payload.size() & 0x7F);
            pong_frame += payload;
            std::lock_guard lock(ws_out_mu_);
            ws_out_queue_.push(pong_frame);
        }
        // Text/binary frames ignored (server is push-only)
    }
}

// ── HttpServer ─────────────────────────────────────────────────────────────

HttpServer::HttpServer(Config cfg, cortex::stream::StatAccumulator& accumulator)
    : cfg_(std::move(cfg))
    , accumulator_(accumulator)
    , poller_(std::make_unique<KqueuePoller>(1024))
{
    // Create TCP listening socket (IPv4 — straightforward, no dual-stack ambiguity)
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

    // Connect to DB (shared, used from poller thread only)
    if (!cfg_.db_conn_str.empty()) {
        try {
            db_ = std::make_unique<pqxx::connection>(cfg_.db_conn_str);
        } catch (const std::exception& e) {
            auto log = cortex::get_logger("http");
            log->warn("DB unavailable: {}", e.what());
        }
    }
}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

void HttpServer::run() {
    auto log = cortex::get_logger("http");
    log->info("HttpServer listening on port {}", cfg_.port);

    // Register accept callback BEFORE setting running_ so callers that poll
    // running() are guaranteed the poller is ready to accept connections.
    poller_->add(listen_fd_, IOEvent::Readable,
                 [this](int /*fd*/, IOEvent /*ev*/) { accept_connection(); });

    running_.store(true, std::memory_order_release);

    poller_->run();

    running_.store(false);
    log->info("HttpServer stopped");
}

void HttpServer::stop() {
    poller_->stop();
}

void HttpServer::accept_connection() {
    while (true) {
        struct sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        int cfd = ::accept(listen_fd_,
                           reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }

        set_nonblock(cfd);
        set_tcp_nodelay(cfd);

        auto conn = std::make_unique<Connection>(cfd, *poller_);
        conn->accumulator = &accumulator_;
        conn->db_conn     = db_.get();

        Connection* raw = conn.get();

        {
            std::lock_guard lock(conn_mu_);
            conns_[cfd] = std::move(conn);
        }

        poller_->add(cfd, IOEvent::Readable,
                     [this, raw](int /*fd*/, IOEvent ev) {
                         if (ev & IOEvent::Readable) raw->on_readable();
                         if (ev & IOEvent::Writable) raw->on_writable();
                         if ((ev & IOEvent::Error) || (ev & IOEvent::HangUp)
                             || raw->closed()) {
                             reap_closed();
                         }
                     });
    }
}

void HttpServer::reap_closed() {
    std::lock_guard lock(conn_mu_);
    for (auto it = conns_.begin(); it != conns_.end(); ) {
        if (it->second->closed())
            it = conns_.erase(it);
        else
            ++it;
    }
}

void HttpServer::broadcast(const std::string& game_id, std::string payload) {
    std::lock_guard lock(conn_mu_);
    for (auto& [fd, conn] : conns_) {
        if (conn->is_ws() && conn->subscribed_game() == game_id)
            conn->ws_send(payload);
    }
}

} // namespace cortex::serving
