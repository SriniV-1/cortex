#include "serving/HttpServer.hpp"
#if defined(__APPLE__)
#  include "serving/KqueuePoller.hpp"
   using PlatformPoller = cortex::serving::KqueuePoller;
#else
#  include "serving/EpollPoller.hpp"
   using PlatformPoller = cortex::serving::EpollPoller;
#endif
#include "serving/RedisCache.hpp"
#include "analytics/GameStateIndex.hpp"
#include "analytics/EloTracker.hpp"
#include "etl/LiveIngestor.hpp"
#include "common/Logger.hpp"

#include <llhttp.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>

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

// Guess MIME type from file extension
static std::string mime_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size()-5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 3 && path.substr(path.size()-3) == ".js")   return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".css")  return "text/css";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".ico")  return "image/x-icon";
    return "application/octet-stream";
}

// Read a local file into string. Returns nullopt if not found / not readable.
static std::optional<std::string> read_file(const std::string& filepath) {
    // Reject path traversal
    if (filepath.find("..") != std::string::npos) return std::nullopt;
    struct stat st{};
    if (::stat(filepath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
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

    // ── Rate limiting (skip /health and /metrics — operational endpoints)
    if (rate_limiter && !client_ip.empty()) {
        if (!rate_limiter->allow(client_ip)) {
            send_response(429, "application/json",
                R"({"error":"rate limit exceeded — try again later"})", false);
            return;
        }
    }

    // ── /metrics (Prometheus) ────────────────────────────────────────────
    if (method == "GET" && path == "/metrics") {
        std::ostringstream m;
        m << "# HELP cortex_events_processed Total stream events processed\n"
          << "# TYPE cortex_events_processed counter\n";
        if (accumulator)
            m << "cortex_events_processed " << accumulator->event_count() << "\n";

        m << "# HELP cortex_active_games Number of games currently tracked in accumulator\n"
          << "# TYPE cortex_active_games gauge\n";
        if (accumulator)
            m << "cortex_active_games " << accumulator->game_count() << "\n";

        m << "# HELP cortex_rate_limiter_buckets Number of active rate limiter buckets\n"
          << "# TYPE cortex_rate_limiter_buckets gauge\n";
        if (rate_limiter)
            m << "cortex_rate_limiter_buckets " << rate_limiter->bucket_count() << "\n";

        m << "# HELP cortex_similarity_index_size Number of vectors in similarity index\n"
          << "# TYPE cortex_similarity_index_size gauge\n";
        if (game_state_index && game_state_index->loaded())
            m << "cortex_similarity_index_size " << game_state_index->size() << "\n";
        else
            m << "cortex_similarity_index_size 0\n";

        m << "# HELP cortex_elo_games_processed Total games used for Elo computation\n"
          << "# TYPE cortex_elo_games_processed gauge\n";
        if (elo_tracker && elo_tracker->built())
            m << "cortex_elo_games_processed " << elo_tracker->games_processed() << "\n";
        else
            m << "cortex_elo_games_processed 0\n";

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

        // Cache-aside: try Redis first (1-min TTL for live game data)
        const std::string cache_key = "cortex:stats:" + game_id;
        if (cache) {
            auto cached = cache->get(cache_key);
            if (cached) {
                send_response(200, "application/json", *cached, false);
                return;
            }
        }

        auto [home, away] = accumulator->score(game_id);
        std::ostringstream j;
        j << "{\"game_id\":" << json_str(game_id)
          << ",\"score_home\":" << home
          << ",\"score_away\":" << away
          << ",\"events_processed\":" << accumulator->event_count()
          << "}";
        const std::string body = j.str();

        // Store in Redis for 60s
        if (cache) cache->set(cache_key, body, std::chrono::seconds{60});

        send_response(200, "application/json", body, false);
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

    // ── /api/stats — total counts for dashboard chips ────────────────────
    if (method == "GET" && path == "/api/stats") {
        if (!db_conn) {
            send_response(500, "application/json", R"({"error":"no db"})", false);
            return;
        }
        try {
            pqxx::work txn(*db_conn);
            auto r = txn.exec(
                "SELECT "
                "  (SELECT COUNT(*) FROM games)         AS total_games, "
                "  (SELECT COUNT(*) FROM players)       AS total_players, "
                "  (SELECT COUNT(*) FROM play_events)   AS total_events");
            txn.commit();
            std::ostringstream j;
            j << "{"
              << "\"total_games\":"   << r[0]["total_games"].as<int64_t>(0)   << ","
              << "\"total_players\":" << r[0]["total_players"].as<int64_t>(0) << ","
              << "\"total_events\":"  << r[0]["total_events"].as<int64_t>(0)
              << "}";
            send_response(200, "application/json", j.str(), false);
        } catch (const std::exception& e) {
            log->error("stats DB error: {}", e.what());
            send_response(500, "application/json", R"({"error":"db error"})", false);
        }
        return;
    }

    // ── /api/leaderboard — top players by selected stat ────────────────
    // ?stat=ppg (default) | rpg | spg | bpg | fg_pct | ft_pct
    if (method == "GET" && (path == "/api/leaderboard" ||
                            path.rfind("/api/leaderboard?", 0) == 0)) {
        if (!db_conn) {
            send_response(500, "application/json", R"({"error":"no db"})", false);
            return;
        }

        // Parse ?stat= from query string
        std::string stat = "ppg";  // default
        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            std::string qs = path.substr(qpos + 1);
            auto sp = qs.find("stat=");
            if (sp != std::string::npos) {
                sp += 5;
                auto ep = qs.find('&', sp);
                stat = (ep == std::string::npos) ? qs.substr(sp) : qs.substr(sp, ep - sp);
            }
        }

        // Whitelist of allowed sort columns → SQL expression
        std::string order_col = "ppg";
        if      (stat == "rpg")    order_col = "rpg";
        else if (stat == "spg")    order_col = "spg";
        else if (stat == "bpg")    order_col = "bpg";
        else if (stat == "fg_pct") order_col = "fg_pct";
        else if (stat == "ft_pct") order_col = "ft_pct";
        // ppg is the default, already set

        try {
            pqxx::work txn(*db_conn);
            auto r = txn.exec(
                "SELECT p.player_id, p.first_name || ' ' || p.last_name AS name, "
                "       t.tricode AS team, COALESCE(p.position, '') AS position, "
                "       SUM(pgs.points) AS pts, "
                "       SUM(pgs.rebounds) AS reb, "
                "       SUM(pgs.steals) AS stl, "
                "       SUM(pgs.blocks) AS blk, "
                "       COUNT(DISTINCT pgs.game_id) AS games, "
                "       ROUND(SUM(pgs.points)::numeric   / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS ppg, "
                "       ROUND(SUM(pgs.rebounds)::numeric  / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS rpg, "
                "       ROUND(SUM(pgs.steals)::numeric    / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS spg, "
                "       ROUND(SUM(pgs.blocks)::numeric    / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS bpg, "
                "       ROUND(SUM(pgs.fgm)::numeric * 100 / NULLIF(SUM(pgs.fga),0), 1) AS fg_pct, "
                "       ROUND(SUM(pgs.ftm)::numeric * 100 / NULLIF(SUM(pgs.fta),0), 1) AS ft_pct "
                "FROM player_game_stats pgs "
                "JOIN players p ON p.player_id = pgs.player_id "
                "JOIN teams t   ON t.team_id = p.team_id "
                "GROUP BY p.player_id, name, t.tricode, p.position "
                "HAVING COUNT(DISTINCT pgs.game_id) >= 30 "
                "ORDER BY " + order_col + " DESC NULLS LAST LIMIT 50");
            txn.commit();

            std::ostringstream j;
            j << "{\"stat\":" << json_str(stat) << ",\"players\":[";
            for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
                if (i > 0) j << ",";
                j << "{"
                  << "\"rank\":"      << (i+1) << ","
                  << "\"player_id\":" << r[i]["player_id"].as<int>() << ","
                  << "\"name\":"      << json_str(r[i]["name"].as<std::string>()) << ","
                  << "\"team\":"      << json_str(r[i]["team"].as<std::string>()) << ","
                  << "\"pos\":"       << json_str(r[i]["position"].as<std::string>("")) << ","
                  << "\"games\":"     << r[i]["games"].as<int>(0) << ","
                  << "\"pts\":"       << r[i]["pts"].as<int>(0) << ","
                  << "\"reb\":"       << r[i]["reb"].as<int>(0) << ","
                  << "\"stl\":"       << r[i]["stl"].as<int>(0) << ","
                  << "\"blk\":"       << r[i]["blk"].as<int>(0) << ","
                  << "\"ppg\":"       << r[i]["ppg"].as<double>(0.0) << ","
                  << "\"rpg\":"       << r[i]["rpg"].as<double>(0.0) << ","
                  << "\"spg\":"       << r[i]["spg"].as<double>(0.0) << ","
                  << "\"bpg\":"       << r[i]["bpg"].as<double>(0.0) << ","
                  << "\"fg_pct\":"    << r[i]["fg_pct"].as<double>(0.0) << ","
                  << "\"ft_pct\":"    << r[i]["ft_pct"].as<double>(0.0)
                  << "}";
            }
            j << "]}";
            send_response(200, "application/json", j.str(), false);
        } catch (const std::exception& e) {
            log->error("leaderboard DB error: {}", e.what());
            send_response(500, "application/json", R"({"error":"db error"})", false);
        }
        return;
    }

    // ── /api/players/search?q= — search players by name ────────────────
    {
        auto qpos = path.find('?');
        std::string path_only = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

        if (method == "GET" && path_only == "/api/players/search") {
            if (!db_conn) {
                send_response(500, "application/json", R"({"error":"no db"})", false);
                return;
            }

            std::string query;
            if (qpos != std::string::npos) {
                std::string qs = path.substr(qpos + 1);
                auto sp = qs.find("q=");
                if (sp != std::string::npos) {
                    sp += 2;
                    auto ep = qs.find('&', sp);
                    query = (ep == std::string::npos) ? qs.substr(sp) : qs.substr(sp, ep - sp);
                    // URL-decode %20 → space (minimal decode for names)
                    std::string decoded;
                    for (size_t i = 0; i < query.size(); ++i) {
                        if (query[i] == '+') decoded += ' ';
                        else if (query[i] == '%' && i + 2 < query.size()) {
                            int val = 0;
                            try { val = std::stoi(query.substr(i+1, 2), nullptr, 16); } catch (...) {}
                            decoded += static_cast<char>(val);
                            i += 2;
                        } else decoded += query[i];
                    }
                    query = decoded;
                }
            }

            if (query.empty() || query.size() < 2) {
                send_response(400, "application/json",
                    R"({"error":"query 'q' must be at least 2 characters"})", false);
                return;
            }

            try {
                pqxx::work txn(*db_conn);
                auto r = txn.exec(
                    "SELECT p.player_id, p.first_name || ' ' || p.last_name AS name, "
                    "       t.tricode AS team, COALESCE(p.position, '') AS position, "
                    "       COALESCE(SUM(pgs.points), 0) AS pts, "
                    "       COALESCE(SUM(pgs.rebounds), 0) AS reb, "
                    "       COALESCE(SUM(pgs.steals), 0) AS stl, "
                    "       COALESCE(SUM(pgs.blocks), 0) AS blk, "
                    "       COUNT(DISTINCT pgs.game_id) AS games, "
                    "       ROUND(COALESCE(SUM(pgs.points), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS ppg, "
                    "       ROUND(COALESCE(SUM(pgs.rebounds), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS rpg, "
                    "       ROUND(COALESCE(SUM(pgs.steals), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS spg, "
                    "       ROUND(COALESCE(SUM(pgs.blocks), 0)::numeric / NULLIF(COUNT(DISTINCT pgs.game_id),0), 1) AS bpg, "
                    "       ROUND(COALESCE(SUM(pgs.fgm), 0)::numeric * 100 / NULLIF(COALESCE(SUM(pgs.fga), 0),0), 1) AS fg_pct, "
                    "       ROUND(COALESCE(SUM(pgs.ftm), 0)::numeric * 100 / NULLIF(COALESCE(SUM(pgs.fta), 0),0), 1) AS ft_pct "
                    "FROM players p "
                    "LEFT JOIN teams t ON t.team_id = p.team_id "
                    "LEFT JOIN player_game_stats pgs ON pgs.player_id = p.player_id "
                    "WHERE (p.first_name || ' ' || p.last_name) ILIKE '%' || $1 || '%' "
                    "GROUP BY p.player_id, name, t.tricode, p.position "
                    "ORDER BY COALESCE(SUM(pgs.points), 0) DESC "
                    "LIMIT 25",
                    pqxx::params{query});
                txn.commit();

                std::ostringstream j;
                j << "{\"query\":" << json_str(query) << ",\"players\":[";
                for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
                    if (i > 0) j << ",";
                    j << "{"
                      << "\"player_id\":" << r[i]["player_id"].as<int>() << ","
                      << "\"name\":"      << json_str(r[i]["name"].as<std::string>()) << ","
                      << "\"team\":"      << json_str(r[i]["team"].as<std::string>("")) << ","
                      << "\"pos\":"       << json_str(r[i]["position"].as<std::string>("")) << ","
                      << "\"games\":"     << r[i]["games"].as<int>(0) << ","
                      << "\"pts\":"       << r[i]["pts"].as<int>(0) << ","
                      << "\"reb\":"       << r[i]["reb"].as<int>(0) << ","
                      << "\"stl\":"       << r[i]["stl"].as<int>(0) << ","
                      << "\"blk\":"       << r[i]["blk"].as<int>(0) << ","
                      << "\"ppg\":"       << r[i]["ppg"].as<double>(0.0) << ","
                      << "\"rpg\":"       << r[i]["rpg"].as<double>(0.0) << ","
                      << "\"spg\":"       << r[i]["spg"].as<double>(0.0) << ","
                      << "\"bpg\":"       << r[i]["bpg"].as<double>(0.0) << ","
                      << "\"fg_pct\":"    << r[i]["fg_pct"].as<double>(0.0) << ","
                      << "\"ft_pct\":"    << r[i]["ft_pct"].as<double>(0.0)
                      << "}";
                }
                j << "]}";
                send_response(200, "application/json", j.str(), false);
            } catch (const std::exception& e) {
                log->error("player search DB error: {}", e.what());
                send_response(500, "application/json", R"({"error":"db error"})", false);
            }
            return;
        }
    }

    // ── /api/games/search?team= — search games by team tricode ──────────
    {
        auto qpos = path.find('?');
        std::string path_only = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

        if (method == "GET" && path_only == "/api/games/search") {
            if (!db_conn) {
                send_response(500, "application/json", R"({"error":"no db"})", false);
                return;
            }

            std::string team;
            if (qpos != std::string::npos) {
                std::string qs = path.substr(qpos + 1);
                auto sp = qs.find("team=");
                if (sp != std::string::npos) {
                    sp += 5;
                    auto ep = qs.find('&', sp);
                    team = (ep == std::string::npos) ? qs.substr(sp) : qs.substr(sp, ep - sp);
                }
            }

            if (team.empty()) {
                send_response(400, "application/json",
                    R"json({"error":"query 'team' required (e.g. BOS, LAL)"})json", false);
                return;
            }

            // Uppercase the tricode
            std::transform(team.begin(), team.end(), team.begin(), ::toupper);

            try {
                pqxx::work txn(*db_conn);
                auto r = txn.exec(
                    "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, "
                    "       g.status, g.season_type, "
                    "       ht.tricode AS home, at.tricode AS away, "
                    "       ht.full_name AS home_name, at.full_name AS away_name "
                    "FROM games g "
                    "JOIN teams ht ON ht.team_id = g.home_team_id "
                    "JOIN teams at ON at.team_id = g.away_team_id "
                    "WHERE ht.tricode = $1 OR at.tricode = $1 "
                    "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 50",
                    pqxx::params{team});
                txn.commit();

                std::ostringstream j;
                j << "{\"team\":" << json_str(team) << ",\"games\":[";
                for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
                    if (i > 0) j << ",";
                    j << "{"
                      << "\"game_id\":"     << json_str(r[i]["game_id"].as<std::string>()) << ","
                      << "\"date\":"        << json_str(r[i]["game_date"].as<std::string>()) << ","
                      << "\"home\":"        << json_str(r[i]["home"].as<std::string>()) << ","
                      << "\"away\":"        << json_str(r[i]["away"].as<std::string>()) << ","
                      << "\"home_name\":"   << json_str(r[i]["home_name"].as<std::string>()) << ","
                      << "\"away_name\":"   << json_str(r[i]["away_name"].as<std::string>()) << ","
                      << "\"home_score\":"  << r[i]["home_score"].as<int>(0) << ","
                      << "\"away_score\":"  << r[i]["away_score"].as<int>(0) << ","
                      << "\"status\":"      << r[i]["status"].as<int>(1) << ","
                      << "\"season_type\":" << json_str(r[i]["season_type"].as<std::string>())
                      << "}";
                }
                j << "]}";
                send_response(200, "application/json", j.str(), false);
            } catch (const std::exception& e) {
                log->error("games search DB error: {}", e.what());
                send_response(500, "application/json", R"({"error":"db error"})", false);
            }
            return;
        }
    }

    // ── /api/events/search — search play events ─────────────────────────
    // ?player_id=203999&action_type=3pt&limit=50
    {
        auto qpos = path.find('?');
        std::string path_only = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

        if (method == "GET" && path_only == "/api/events/search") {
            if (!db_conn) {
                send_response(500, "application/json", R"({"error":"no db"})", false);
                return;
            }

            std::string qs = (qpos != std::string::npos) ? path.substr(qpos + 1) : "";
            auto parse_param = [&](const std::string& key) -> std::string {
                const std::string needle = key + "=";
                auto pos = qs.find(needle);
                while (pos != std::string::npos) {
                    if (pos == 0 || qs[pos - 1] == '&') break;
                    pos = qs.find(needle, pos + 1);
                }
                if (pos == std::string::npos) return "";
                pos += key.size() + 1;
                auto end = qs.find('&', pos);
                return (end == std::string::npos) ? qs.substr(pos) : qs.substr(pos, end - pos);
            };

            std::string pid_str    = parse_param("player_id");
            std::string action     = parse_param("action_type");
            std::string game_id    = parse_param("game_id");
            std::string limit_str  = parse_param("limit");

            int limit_n = 50;
            if (!limit_str.empty()) {
                try { limit_n = std::max(1, std::min(std::stoi(limit_str), 200)); } catch (...) {}
            }

            if (pid_str.empty() && action.empty() && game_id.empty()) {
                send_response(400, "application/json",
                    R"({"error":"provide at least one of: player_id, action_type, game_id"})", false);
                return;
            }

            // Build parameterized query dynamically
            std::string sql =
                "SELECT pe.event_id, pe.game_id, pe.action_number, "
                "       pe.occurred_at::text, pe.period, pe.clock, "
                "       pe.action_type, pe.sub_type, pe.description, "
                "       pe.player_id, pe.team_id, pe.score_home, pe.score_away, "
                "       p.first_name || ' ' || p.last_name AS player_name "
                "FROM play_events pe "
                "LEFT JOIN players p ON p.player_id = pe.player_id "
                "WHERE 1=1 ";

            // We build conditions and use string params to keep it safe
            // pqxx positional params: $1, $2, etc.
            std::vector<std::string> conditions;

            // Whitelist action_type values
            static const std::vector<std::string> valid_actions = {
                "2pt", "3pt", "freethrow", "rebound", "steal", "block",
                "turnover", "foul", "substitution", "timeout", "jumpball",
                "period", "violation", "ejection"
            };

            int pid_val = 0;
            if (!pid_str.empty()) {
                try { pid_val = std::stoi(pid_str); } catch (...) {}
            }

            try {
                pqxx::work txn(*db_conn);
                pqxx::result r;

                if (!pid_str.empty() && !action.empty() && !game_id.empty()) {
                    bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
                    if (!valid) { send_response(400, "application/json", R"({"error":"invalid action_type"})", false); return; }
                    r = txn.exec(
                        sql + "AND pe.player_id = $1 AND pe.action_type = $2 AND pe.game_id = $3 "
                        "ORDER BY pe.occurred_at DESC LIMIT $4",
                        pqxx::params{pid_val, action, game_id, limit_n});
                } else if (!pid_str.empty() && !action.empty()) {
                    bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
                    if (!valid) { send_response(400, "application/json", R"({"error":"invalid action_type"})", false); return; }
                    r = txn.exec(
                        sql + "AND pe.player_id = $1 AND pe.action_type = $2 "
                        "ORDER BY pe.occurred_at DESC LIMIT $3",
                        pqxx::params{pid_val, action, limit_n});
                } else if (!pid_str.empty() && !game_id.empty()) {
                    r = txn.exec(
                        sql + "AND pe.player_id = $1 AND pe.game_id = $2 "
                        "ORDER BY pe.action_number ASC LIMIT $3",
                        pqxx::params{pid_val, game_id, limit_n});
                } else if (!game_id.empty() && !action.empty()) {
                    bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
                    if (!valid) { send_response(400, "application/json", R"({"error":"invalid action_type"})", false); return; }
                    r = txn.exec(
                        sql + "AND pe.game_id = $1 AND pe.action_type = $2 "
                        "ORDER BY pe.action_number ASC LIMIT $3",
                        pqxx::params{game_id, action, limit_n});
                } else if (!pid_str.empty()) {
                    r = txn.exec(
                        sql + "AND pe.player_id = $1 ORDER BY pe.occurred_at DESC LIMIT $2",
                        pqxx::params{pid_val, limit_n});
                } else if (!game_id.empty()) {
                    r = txn.exec(
                        sql + "AND pe.game_id = $1 ORDER BY pe.action_number ASC LIMIT $2",
                        pqxx::params{game_id, limit_n});
                } else if (!action.empty()) {
                    bool valid = std::find(valid_actions.begin(), valid_actions.end(), action) != valid_actions.end();
                    if (!valid) { send_response(400, "application/json", R"({"error":"invalid action_type"})", false); return; }
                    r = txn.exec(
                        sql + "AND pe.action_type = $1 ORDER BY pe.occurred_at DESC LIMIT $2",
                        pqxx::params{action, limit_n});
                }
                txn.commit();

                std::ostringstream j;
                j << "{\"count\":" << r.size() << ",\"events\":[";
                for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
                    if (i > 0) j << ",";
                    j << "{"
                      << "\"event_id\":"   << r[i]["event_id"].as<int64_t>() << ","
                      << "\"game_id\":"    << json_str(r[i]["game_id"].as<std::string>()) << ","
                      << "\"action_num\":" << r[i]["action_number"].as<int>() << ","
                      << "\"time\":"       << json_str(r[i]["occurred_at"].as<std::string>()) << ","
                      << "\"period\":"     << r[i]["period"].as<int>() << ","
                      << "\"clock\":"      << json_str(r[i]["clock"].as<std::string>("")) << ","
                      << "\"action\":"     << json_str(r[i]["action_type"].as<std::string>()) << ","
                      << "\"sub_type\":"   << json_str(r[i]["sub_type"].as<std::string>("")) << ","
                      << "\"desc\":"       << json_str(r[i]["description"].as<std::string>("")) << ","
                      << "\"player_id\":" ;
                    if (r[i]["player_id"].is_null()) j << "null"; else j << r[i]["player_id"].as<int>();
                    j << ",\"player_name\":" << json_str(r[i]["player_name"].as<std::string>("")) << ","
                      << "\"score_home\":" << r[i]["score_home"].as<int>(0) << ","
                      << "\"score_away\":" << r[i]["score_away"].as<int>(0)
                      << "}";
                }
                j << "]}";
                send_response(200, "application/json", j.str(), false);
            } catch (const std::exception& e) {
                log->error("events search DB error: {}", e.what());
                send_response(500, "application/json", R"({"error":"db error"})", false);
            }
            return;
        }
    }

    // ── /api/games/recent — last 20 completed games ──────────────────────
    // ?type=regular|playoffs (default: all)
    {
        auto qpos = path.find('?');
        std::string path_only = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

        if (method == "GET" && path_only == "/api/games/recent") {
        if (!db_conn) {
            send_response(500, "application/json", R"({"error":"no db"})", false);
            return;
        }

        // Parse ?type= filter
        std::string type_filter;
        if (qpos != std::string::npos) {
            std::string qs = path.substr(qpos + 1);
            auto sp = qs.find("type=");
            if (sp != std::string::npos) {
                sp += 5;
                auto ep = qs.find('&', sp);
                type_filter = (ep == std::string::npos) ? qs.substr(sp) : qs.substr(sp, ep - sp);
            }
        }

        try {
            pqxx::work txn(*db_conn);
            pqxx::result r;

            if (type_filter == "regular") {
                r = txn.exec(
                    "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
                    "       ht.tricode AS home, at.tricode AS away, "
                    "       ht.full_name AS home_name, at.full_name AS away_name "
                    "FROM games g "
                    "JOIN teams ht ON ht.team_id = g.home_team_id "
                    "JOIN teams at ON at.team_id = g.away_team_id "
                    "WHERE g.season_type = 'Regular Season' "
                    "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 20");
            } else if (type_filter == "playoffs") {
                r = txn.exec(
                    "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
                    "       ht.tricode AS home, at.tricode AS away, "
                    "       ht.full_name AS home_name, at.full_name AS away_name "
                    "FROM games g "
                    "JOIN teams ht ON ht.team_id = g.home_team_id "
                    "JOIN teams at ON at.team_id = g.away_team_id "
                    "WHERE g.season_type = 'Playoffs' "
                    "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 20");
            } else {
                r = txn.exec(
                    "SELECT g.game_id, g.game_date::text, g.home_score, g.away_score, g.status, "
                    "       ht.tricode AS home, at.tricode AS away, "
                    "       ht.full_name AS home_name, at.full_name AS away_name "
                    "FROM games g "
                    "JOIN teams ht ON ht.team_id = g.home_team_id "
                    "JOIN teams at ON at.team_id = g.away_team_id "
                    "ORDER BY g.game_date DESC, g.game_id DESC LIMIT 20");
            }
            txn.commit();

            std::ostringstream j;
            j << "[";
            for (pqxx::result::size_type i = 0; i < r.size(); ++i) {
                if (i > 0) j << ",";
                j << "{"
                  << "\"game_id\":"    << json_str(r[i]["game_id"].as<std::string>()) << ","
                  << "\"date\":"       << json_str(r[i]["game_date"].as<std::string>()) << ","
                  << "\"home\":"       << json_str(r[i]["home"].as<std::string>()) << ","
                  << "\"away\":"       << json_str(r[i]["away"].as<std::string>()) << ","
                  << "\"home_name\":"  << json_str(r[i]["home_name"].as<std::string>()) << ","
                  << "\"away_name\":"  << json_str(r[i]["away_name"].as<std::string>()) << ","
                  << "\"home_score\":" << r[i]["home_score"].as<int>(0) << ","
                  << "\"away_score\":" << r[i]["away_score"].as<int>(0) << ","
                  << "\"status\":"     << r[i]["status"].as<int>(1)
                  << "}";
            }
            j << "]";
            send_response(200, "application/json", j.str(), false);
        } catch (const std::exception& e) {
            log->error("games/recent DB error: {}", e.what());
            send_response(500, "application/json", R"({"error":"db error"})", false);
        }
        return;
        }
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

    // ── /api/scoreboard — today's NBA games (live + scheduled + final) ──
    if (method == "GET" && path == "/api/scoreboard") {
        if (!live_ingestor) {
            send_response(503, "application/json",
                R"json({"error":"live ingestion not enabled -- start with --live"})json", false);
            return;
        }
        auto games = live_ingestor->scoreboard_snapshot();
        std::ostringstream j;
        j << "{\"games\":[";
        for (size_t i = 0; i < games.size(); ++i) {
            if (i > 0) j << ",";
            const auto& g = games[i];
            j << "{"
              << "\"game_id\":"   << json_str(g.game_id) << ","
              << "\"status\":"    << g.status << ","
              << "\"home\":"      << json_str(g.home_tricode) << ","
              << "\"away\":"      << json_str(g.away_tricode) << ","
              << "\"home_score\":" << g.home_score << ","
              << "\"away_score\":" << g.away_score << ","
              << "\"period\":"    << g.period << ","
              << "\"game_clock\":" << json_str(g.game_clock)
              << "}";
        }
        j << "]}";
        send_response(200, "application/json", j.str(), false);
        return;
    }

    // ── /api/elo — team Elo ratings ────────────────────────────────────
    if (method == "GET" && path == "/api/elo") {
        if (!elo_tracker || !elo_tracker->built()) {
            send_response(503, "application/json",
                R"({"error":"Elo ratings not ready yet — building in background"})", false);
            return;
        }
        auto ratings = elo_tracker->all_ratings();
        std::ostringstream j;
        j << "{\"games_processed\":" << elo_tracker->games_processed()
          << ",\"build_ms\":" << std::fixed;
        j.precision(1);
        j << elo_tracker->build_ms()
          << ",\"teams\":[";
        for (size_t i = 0; i < ratings.size(); ++i) {
            if (i > 0) j << ",";
            const auto& te = ratings[i];
            j << "{\"rank\":" << (i + 1)
              << ",\"team_id\":" << te.team_id
              << ",\"tricode\":" << json_str(te.tricode)
              << ",\"rating\":" << std::fixed;
            j.precision(0);
            j << te.rating
              << ",\"games\":" << te.games_played
              << ",\"wins\":" << te.wins
              << ",\"losses\":" << te.losses
              << "}";
        }
        j << "]}";
        send_response(200, "application/json", j.str(), false);
        return;
    }

    // ── /api/elo/history — Elo rating trajectory across seasons ──────────
    if (method == "GET" && path == "/api/elo/history") {
        if (!elo_tracker || !elo_tracker->built()) {
            send_response(503, "application/json",
                R"({"error":"Elo ratings not ready yet"})", false);
            return;
        }
        auto history = elo_tracker->elo_history();
        // Group by season, then by tricode
        std::ostringstream j;
        j << "{\"snapshots\":[";
        for (size_t i = 0; i < history.size(); ++i) {
            if (i > 0) j << ",";
            const auto& s = history[i];
            j << "{\"season\":" << s.season
              << ",\"tricode\":" << json_str(s.tricode)
              << ",\"rating\":" << std::fixed;
            j.precision(0);
            j << s.rating << "}";
        }
        j << "]}";
        send_response(200, "application/json", j.str(), false);
        return;
    }

    // ── /api/index/status — similarity index readiness ───────────────────
    if (method == "GET" && path == "/api/index/status") {
        std::ostringstream j;
        if (game_state_index && game_state_index->loaded()) {
            j << "{\"loaded\":true"
              << ",\"size\":"      << game_state_index->size()
              << ",\"build_ms\":"  << static_cast<long long>(game_state_index->build_ms())
              << "}";
        } else {
            j << "{\"loaded\":false,\"size\":0,\"build_ms\":0}";
        }
        send_response(200, "application/json", j.str(), false);
        return;
    }

    // ── /api/similar — SIMD nearest-neighbor game state search ──────────
    // Query params: score_home, score_away, period, clock (secs remaining),
    //               momentum (optional, default 0), k (optional, default 10)
    {
        auto qpos = path.find('?');
        std::string path_only = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

        if (method == "GET" && path_only == "/api/similar") {
            if (!game_state_index || !game_state_index->loaded()) {
                send_response(503, "application/json",
                    R"({"error":"similarity index not ready yet — building in background"})",
                    false);
                return;
            }

            // Parse query parameters
            auto parse_int_param = [&](const std::string& qs, const std::string& key,
                                       int fallback) -> int {
                const std::string needle = key + "=";
                auto pos = qs.find(needle);
                // Require match at start of string or after '&' to avoid substring collisions
                // e.g. "k=" must not match the "k=" inside "clock=180"
                while (pos != std::string::npos) {
                    if (pos == 0 || qs[pos - 1] == '&') break;
                    pos = qs.find(needle, pos + 1);
                }
                if (pos == std::string::npos) return fallback;
                pos += key.size() + 1;
                auto end = qs.find('&', pos);
                std::string val = (end == std::string::npos)
                    ? qs.substr(pos)
                    : qs.substr(pos, end - pos);
                try { return std::stoi(val); } catch (...) { return fallback; }
            };

            const std::string qs = (qpos != std::string::npos) ? path.substr(qpos + 1) : "";

            const int score_home = parse_int_param(qs, "score_home", 50);
            const int score_away = parse_int_param(qs, "score_away", 50);
            const int period     = parse_int_param(qs, "period",     2);
            const int clock      = parse_int_param(qs, "clock",      360);
            const int momentum   = parse_int_param(qs, "momentum",   0);
            const int k          = std::max(1, std::min(parse_int_param(qs, "k", 10), 25));

            const auto t0 = std::chrono::steady_clock::now();

            auto qvec    = cortex::analytics::encode_game_state(
                               score_home, score_away, period, clock, momentum);
            auto matches = game_state_index->query(qvec, k);

            const auto t1      = std::chrono::steady_clock::now();
            const double qms   = std::chrono::duration<double, std::milli>(t1 - t0).count();

            std::ostringstream j;
            j << "{"
              << "\"query\":{"
              << "\"score_home\":"  << score_home << ","
              << "\"score_away\":"  << score_away << ","
              << "\"period\":"      << period     << ","
              << "\"clock\":"       << clock      << ","
              << "\"momentum\":"    << momentum
              << "},"
              << "\"query_ms\":"    << std::fixed;
            j.precision(2);
            j << qms << ","
              << "\"index_size\":"  << game_state_index->size() << ","
              << "\"results\":[";

            for (size_t ri = 0; ri < matches.size(); ++ri) {
                const auto& m = matches[ri];
                if (ri > 0) j << ",";
                j << "{"
                  << "\"rank\":"       << (ri + 1)          << ","
                  << "\"event_id\":"   << m.event_id        << ","
                  << "\"game_id\":"    << json_str(m.game_id) << ","
                  << "\"home\":"       << json_str(m.home_tricode) << ","
                  << "\"away\":"       << json_str(m.away_tricode) << ","
                  << "\"date\":"       << json_str(m.date)   << ","
                  << "\"score_home\":" << m.score_home       << ","
                  << "\"score_away\":" << m.score_away       << ","
                  << "\"period\":"     << static_cast<int>(m.period) << ","
                  << "\"home_won\":"   << (m.home_won ? "true" : "false") << ","
                  << "\"similarity\":" << std::fixed;
                j.precision(4);
                j << m.similarity
                  << "}";
            }
            j << "]}";

            // Cache similar-moments results for 5 minutes (they rarely change mid-game)
            if (cache) {
                const std::string ckey = "cortex:similar:" + std::to_string(score_home)
                    + ":" + std::to_string(score_away) + ":" + std::to_string(period)
                    + ":" + std::to_string(clock);
                cache->set(ckey, j.str(), std::chrono::seconds{300});
            }

            send_response(200, "application/json", j.str(), false);
            return;
        }
    }

    // ── / and /static/* — serve dashboard files ─────────────────────────
    if (method == "GET") {
        std::string file_path;
        if (path == "/" || path == "/index.html") {
            file_path = www_root + "/index.html";
        } else if (path.rfind("/static/", 0) == 0) {
            // Strip query string if any
            auto qs = path.find('?');
            file_path = www_root + (qs == std::string::npos ? path : path.substr(0, qs));
        }

        if (!file_path.empty()) {
            auto content = read_file(file_path);
            if (content) {
                send_response(200, mime_type(file_path), *content, false);
                return;
            }
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
        if (ws_out_queue_.size() >= kMaxWsQueueFrames) {
            // Client too slow — drop connection to prevent unbounded memory growth.
            auto log = cortex::get_logger("ws");
            log->warn("WebSocket fd={} exceeded {} queued frames — disconnecting slow client",
                       fd_, kMaxWsQueueFrames);
            state_ = State::Closing;
            return;
        }
        ws_out_queue_.push(std::move(frame));
    }
    // Re-register write interest so kqueue fires EVFILT_WRITE to flush.
    // Safe to call from any thread — kevent(2) is thread-safe.
    poller_.modify(fd_, IOEvent::Readable | IOEvent::Writable);
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

    // Redis cache (gracefully degrades if unavailable)
    cache_ = std::make_unique<RedisCache>(cfg_.redis_host, cfg_.redis_port);
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
        conn->accumulator       = &accumulator_;
        conn->db_conn           = db_.get();
        conn->cache             = cache_.get();
        conn->www_root          = cfg_.www_root;
        conn->game_state_index  = cfg_.game_state_index;
        conn->elo_tracker       = cfg_.elo_tracker;
        conn->live_ingestor     = cfg_.live_ingestor;
        conn->rate_limiter      = &rate_limiter_;

        // Extract client IP for rate limiting
        if (peer.ss_family == AF_INET) {
            char ip[INET_ADDRSTRLEN]{};
            auto* sa4 = reinterpret_cast<struct sockaddr_in*>(&peer);
            ::inet_ntop(AF_INET, &sa4->sin_addr, ip, sizeof(ip));
            conn->client_ip = ip;
        }

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
