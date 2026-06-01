#pragma once
// Response — typed wrapper for an outgoing HTTP response.
//
// Handlers fill in status, content_type, and body. The dispatch code in
// Connection::process_http_request translates this into a raw HTTP response.

#include <string>

namespace cortex::serving {

struct Response {
    int         status_code  = 200;
    std::string content_type = "application/json";
    std::string body;
    bool        handled      = false;  // set to true when a handler writes a response
    bool        keep_alive   = false;

    // ── Builder helpers ──────────────────────────────────────────────────

    void json(const std::string& json_body, int status = 200) {
        status_code  = status;
        content_type = "application/json";
        body         = json_body;
        handled      = true;
    }

    void text(const std::string& text_body, int status = 200) {
        status_code  = status;
        content_type = "text/plain; version=0.0.4";
        body         = text_body;
        handled      = true;
    }

    void send_file(const std::string& content,
                   const std::string& mime,
                   int status = 200) {
        status_code  = status;
        content_type = mime;
        body         = content;
        handled      = true;
    }

    void status(int code) {
        status_code = code;
    }

    void error(const std::string& msg, int code = 500) {
        json(R"({"error":")" + msg + R"("})", code);
    }
};

} // namespace cortex::serving
