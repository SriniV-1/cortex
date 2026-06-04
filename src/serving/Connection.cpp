#include "serving/HttpServer.hpp"
#include "serving/HttpUtils.hpp"
#include "serving/Router.hpp"
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"
#include "serving/handlers/static_handler.hpp"
#include "common/Logger.hpp"
#include "common/TraceContext.hpp"

#include <chrono>

#include <llhttp.h>

#include <cerrno>
#include <cstring>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace cortex::serving {

// ── Helpers (used only by Connection) ─────────────────────────────────────

static std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.resize(((len + 2) / 3) * 4 + 1);
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    out.resize(n);
    return out;
}

static std::string ws_accept_key(const std::string& key) {
    static constexpr char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;
    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.data()),
         combined.size(), sha);
    return base64_encode(sha, SHA_DIGEST_LENGTH);
}

// ── llhttp callbacks ──────────────────────────────────────────────────────

static Connection* conn_from(llhttp_t* p) {
    return static_cast<Connection*>(p->data);
}
static int on_url(llhttp_t* p, const char* at, size_t len) {
    conn_from(p)->req_url_.append(at, len); return 0;
}
static int on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* c = conn_from(p);
    if (!c->cur_header_value_.empty()) {
        c->req_headers_raw_ += c->cur_header_field_ + ": " + c->cur_header_value_ + "\n";
        c->cur_header_field_.clear(); c->cur_header_value_.clear();
    }
    c->cur_header_field_.append(at, len); return 0;
}
static int on_header_value(llhttp_t* p, const char* at, size_t len) {
    conn_from(p)->cur_header_value_.append(at, len); return 0;
}
static int on_headers_complete(llhttp_t* p) {
    auto* c = conn_from(p);
    if (!c->cur_header_field_.empty())
        c->req_headers_raw_ += c->cur_header_field_ + ": " + c->cur_header_value_ + "\n";
    c->cur_header_field_.clear(); c->cur_header_value_.clear();
    c->headers_complete_ = true; return 0;
}
static int on_body(llhttp_t* p, const char* at, size_t len) {
    conn_from(p)->req_body_.append(at, len); return 0;
}
static int on_message_complete(llhttp_t* p) {
    auto* c = conn_from(p);
    const char* method_str = llhttp_method_name(static_cast<llhttp_method_t>(p->method));
    c->process_http_request(method_str, c->req_url_, c->req_headers_raw_, c->req_body_);
    c->req_url_.clear(); c->req_headers_raw_.clear(); c->req_body_.clear();
    c->headers_complete_ = false; return 0;
}

// ── Connection lifecycle ──────────────────────────────────────────────────

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
    if (fd_ >= 0) { poller_.remove(fd_); ::close(fd_); fd_ = -1; }
}

void Connection::close() { state_ = State::Closing; }

// ── I/O callbacks ─────────────────────────────────────────────────────────

void Connection::on_readable() {
    char buf[16384];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            if (state_ == State::WebSocket) {
                read_buf_.append(buf, n); ws_parse_incoming();
            } else {
                enum llhttp_errno err = llhttp_execute(parser_.get(), buf, static_cast<size_t>(n));
                if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) { close(); return; }
            }
        } else if (n == 0) { close(); return; }
        else { if (errno == EAGAIN || errno == EWOULDBLOCK) break; close(); return; }
    }
}

void Connection::on_writable() {
    if (state_ == State::WebSocket) { ws_flush_outbound(); return; }
    while (!write_buf_.empty()) {
        ssize_t n = ::write(fd_, write_buf_.data(), write_buf_.size());
        if (n > 0) { write_buf_.erase(0, static_cast<size_t>(n)); }
        else if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; close(); return; }
    }
    if (write_buf_.empty()) {
        if (close_after_flush_) { close(); return; }
        poller_.modify(fd_, IOEvent::Readable);
    }
}

// ── HTTP response ─────────────────────────────────────────────────────────

void Connection::send_response(int status, const std::string& ct,
                                const std::string& body, bool keep_alive) {
    std::string reason;
    switch (status) {
    case 200: reason = "OK";          break;
    case 400: reason = "Bad Request"; break;
    case 404: reason = "Not Found";   break;
    case 429: reason = "Too Many Requests"; break;
    case 500: reason = "Server Error";break;
    case 503: reason = "Service Unavailable"; break;
    default:  reason = "Unknown";     break;
    }
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << reason << "\r\n"
        << "Content-Type: "   << ct        << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: "     << (keep_alive ? "keep-alive" : "close") << "\r\n"
        << "X-Trace-Id: "    << current_trace_id_ << "\r\n"
        << "\r\n" << body;
    write_buf_ += oss.str();
    if (!keep_alive) close_after_flush_ = true;
    poller_.modify(fd_, IOEvent::Readable | IOEvent::Writable);

    // Log request completion with timing
    auto elapsed = std::chrono::steady_clock::now() - request_start_;
    double duration_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    auto log = cortex::get_logger("http");
    log->info("{} {} {} {:.2f}ms trace_id={}",
              current_method_, current_path_, status, duration_ms, current_trace_id_);
}

// ── HTTP request dispatch via Router ──────────────────────────────────────

void Connection::process_http_request(const std::string& method,
                                       const std::string& raw_path,
                                       const std::string& headers_raw,
                                       const std::string& body) {
    // Set up per-request trace context before any early returns
    auto trace = cortex::TraceContext::create();
    current_trace_id_ = trace.trace_id;
    current_method_   = method;
    current_path_     = raw_path;
    request_start_    = std::chrono::steady_clock::now();

    auto log = cortex::get_logger("http");
    log->debug("{} {} trace_id={}", method, raw_path, current_trace_id_);

    // Split path and query string
    std::string path = raw_path;
    std::string query_string;
    auto qpos = raw_path.find('?');
    if (qpos != std::string::npos) {
        path = raw_path.substr(0, qpos);
        query_string = raw_path.substr(qpos + 1);
    }

    // Build ServerContext
    ServerContext ctx;
    ctx.accumulator       = accumulator;
    ctx.db                = db_conn;
    ctx.cache             = cache;
    ctx.game_state_index  = game_state_index;
    ctx.elo_tracker       = elo_tracker;
    ctx.live_ingestor     = live_ingestor;
    ctx.rate_limiter      = rate_limiter;
    ctx.redis_circuit_breaker = redis_circuit_breaker;
    ctx.www_root          = www_root;
    ctx.connection        = this;

    // Try router match
    if (router) {
        auto route = router->match(method, path);
        if (route) {
            // Rate limiting — skip for /health and /metrics
            if (path != "/health" && path != "/metrics") {
                if (rate_limiter && !client_ip.empty()) {
                    if (!rate_limiter->allow(client_ip)) {
                        send_response(429, "application/json",
                            R"({"error":"rate limit exceeded — try again later"})", false);
                        return;
                    }
                }
            }

            Request req;
            req.method       = method;
            req.path         = path;
            req.full_url     = raw_path;
            req.path_params  = std::move(route->params);
            req.query_params = parse_query_string(query_string);
            req.headers_raw  = headers_raw;
            req.body         = body;
            req.client_ip    = client_ip;
            req.trace_id     = current_trace_id_;

            Response res;
            route->handler(req, res, ctx);

            if (res.handled) {
                // If the handler already sent the response directly (e.g. ws upgrade),
                // don't double-send.
                if (!res.body.empty()) {
                    send_response(res.status_code, res.content_type, res.body, res.keep_alive);
                }
            }
            return;
        }
    }

    // Fallback: static file serving for unmatched GET requests
    if (method == "GET") {
        Request req;
        req.method       = method;
        req.path         = path;
        req.full_url     = raw_path;
        req.query_params = parse_query_string(query_string);
        req.trace_id     = current_trace_id_;

        Response res;
        handlers::handle_static(req, res, ctx);
        if (res.handled) {
            send_response(res.status_code, res.content_type, res.body, res.keep_alive);
            return;
        }
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

std::string Connection::ws_encode_frame(const std::string& payload) {
    std::string frame;
    frame += static_cast<char>(0x81);
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
        if (ws_out_queue_.size() >= kMaxWsQueueFrames) {
            auto log = cortex::get_logger("ws");
            log->warn("WebSocket fd={} exceeded {} queued frames — disconnecting slow client",
                       fd_, kMaxWsQueueFrames);
            state_ = State::Closing;
            return;
        }
        ws_out_queue_.push(std::move(frame));
    }
    poller_.modify(fd_, IOEvent::Readable | IOEvent::Writable);
}

void Connection::ws_flush_outbound() {
    {
        std::lock_guard lock(ws_out_mu_);
        while (!ws_out_queue_.empty()) {
            write_buf_ += ws_out_queue_.front(); ws_out_queue_.pop();
        }
    }
    while (!write_buf_.empty()) {
        ssize_t n = ::write(fd_, write_buf_.data(), write_buf_.size());
        if (n > 0) { write_buf_.erase(0, static_cast<size_t>(n)); }
        else if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; close(); return; }
        else { close(); return; }
    }
    if (write_buf_.empty()) poller_.modify(fd_, IOEvent::Readable);
}

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
                payload_len = (payload_len << 8) | static_cast<uint8_t>(read_buf_[2 + i]);
            header_len += 8;
        }
        if (read_buf_.size() < header_len + payload_len) break;
        std::string payload(read_buf_.data() + header_len, payload_len);
        if (masked) {
            const char* mask = read_buf_.data() + header_len - 4;
            for (size_t i = 0; i < payload_len; ++i) payload[i] ^= mask[i % 4];
        }
        read_buf_.erase(0, header_len + payload_len);
        if (opcode == 0x8) {
            ws_send(std::string("\x03\xE8", 2)); close(); return;
        }
        if (opcode == 0x9) {
            std::string pong_frame;
            pong_frame += static_cast<char>(0x8A);
            pong_frame += static_cast<char>(payload.size() & 0x7F);
            pong_frame += payload;
            std::lock_guard lock(ws_out_mu_);
            ws_out_queue_.push(pong_frame);
        }
    }
}

} // namespace cortex::serving
