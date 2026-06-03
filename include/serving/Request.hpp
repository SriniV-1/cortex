#pragma once
// Request — typed wrapper for an incoming HTTP request.

#include <string>
#include <unordered_map>

namespace cortex::serving {

struct Request {
    std::string method;
    std::string path;           // path without query string
    std::string full_url;       // original URL including query string
    std::unordered_map<std::string, std::string> path_params;
    std::unordered_map<std::string, std::string> query_params;
    std::string headers_raw;
    std::string body;
    std::string client_ip;
    std::string trace_id;           // UUID v4 request trace ID

    // Convenience: get a query param with fallback.
    std::string query(const std::string& key,
                      const std::string& fallback = "") const {
        auto it = query_params.find(key);
        return (it != query_params.end()) ? it->second : fallback;
    }

    // Convenience: get a path param with fallback.
    std::string param(const std::string& key,
                      const std::string& fallback = "") const {
        auto it = path_params.find(key);
        return (it != path_params.end()) ? it->second : fallback;
    }

    // Parse a query param as int with fallback.
    int query_int(const std::string& key, int fallback) const {
        auto it = query_params.find(key);
        if (it == query_params.end() || it->second.empty()) return fallback;
        try { return std::stoi(it->second); } catch (...) { return fallback; }
    }
};

} // namespace cortex::serving
