#pragma once
// HttpUtils — shared utility functions for HTTP handlers.
//
// Extracted from HttpServer.cpp so all handler translation units can use them.

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

namespace cortex::serving {

// JSON-escape a string value (adds surrounding quotes).
inline std::string json_str(const std::string& s) {
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

// Extract a header value (case-insensitive field name).
inline std::string get_header(const std::string& headers_raw,
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
            size_t s = v.find_first_not_of(" \t\r\n");
            size_t e = v.find_last_not_of(" \t\r\n");
            return (s == std::string::npos) ? "" : v.substr(s, e - s + 1);
        }
    }
    return "";
}

// Guess MIME type from file extension.
inline std::string mime_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size()-5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 3 && path.substr(path.size()-3) == ".js")   return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".css")  return "text/css";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".ico")  return "image/x-icon";
    return "application/octet-stream";
}

// Read a local file into string. Returns nullopt if not found / not readable.
inline std::optional<std::string> read_file(const std::string& filepath) {
    if (filepath.find("..") != std::string::npos) return std::nullopt;
    struct stat st{};
    if (::stat(filepath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// Parse query string "key1=val1&key2=val2" into a map.
inline std::unordered_map<std::string, std::string>
parse_query_string(const std::string& qs) {
    std::unordered_map<std::string, std::string> params;
    if (qs.empty()) return params;

    size_t pos = 0;
    while (pos < qs.size()) {
        auto amp = qs.find('&', pos);
        std::string pair = (amp == std::string::npos)
            ? qs.substr(pos)
            : qs.substr(pos, amp - pos);
        pos = (amp == std::string::npos) ? qs.size() : amp + 1;

        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            params[pair] = "";
        } else {
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
    }
    return params;
}

// Minimal URL-decode: handles %XX and +.
inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int val = 0;
            try { val = std::stoi(s.substr(i+1, 2), nullptr, 16); } catch (...) {}
            out += static_cast<char>(val);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace cortex::serving
