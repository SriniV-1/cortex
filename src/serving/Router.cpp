#include "serving/Router.hpp"

namespace cortex::serving {

std::vector<std::string> Router::split_path(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0;
    // Skip leading slash
    if (!path.empty() && path[0] == '/') start = 1;

    while (start < path.size()) {
        auto slash = path.find('/', start);
        if (slash == std::string::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        if (slash > start)
            segments.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return segments;
}

void Router::add(const std::string& method,
                 const std::string& pattern,
                 HandlerFn handler) {
    auto segments = split_path(pattern);
    TrieNode* node = &root_;

    for (const auto& seg : segments) {
        if (!seg.empty() && seg[0] == ':') {
            // Parameter segment
            if (!node->param_child) {
                node->param_child = std::make_unique<TrieNode>();
                node->param_name  = seg.substr(1);  // strip ':'
            }
            node = node->param_child.get();
        } else {
            // Literal segment
            auto it = node->children.find(seg);
            if (it == node->children.end()) {
                node->children[seg] = std::make_unique<TrieNode>();
            }
            node = node->children[seg].get();
        }
    }

    node->handlers[method] = std::move(handler);
}

std::optional<RouteMatch> Router::match(const std::string& method,
                                         const std::string& path) const {
    auto segments = split_path(path);
    std::unordered_map<std::string, std::string> params;

    if (match_impl(root_, segments, 0, params)) {
        // Find the terminal node again to get the handler
        const TrieNode* node = &root_;
        for (size_t i = 0; i < segments.size(); ++i) {
            auto it = node->children.find(segments[i]);
            if (it != node->children.end()) {
                node = it->second.get();
            } else if (node->param_child) {
                node = node->param_child.get();
            } else {
                return std::nullopt;
            }
        }

        auto hit = node->handlers.find(method);
        if (hit != node->handlers.end()) {
            return RouteMatch{hit->second, std::move(params)};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool Router::match_impl(const TrieNode& node,
                         const std::vector<std::string>& segments,
                         size_t idx,
                         std::unordered_map<std::string, std::string>& params) const {
    if (idx == segments.size()) {
        // We've consumed all segments — check if this node has any handlers
        return !node.handlers.empty();
    }

    const auto& seg = segments[idx];

    // Try literal match first (higher priority)
    auto it = node.children.find(seg);
    if (it != node.children.end()) {
        if (match_impl(*it->second, segments, idx + 1, params))
            return true;
    }

    // Try parameter match
    if (node.param_child) {
        params[node.param_name] = seg;
        if (match_impl(*node.param_child, segments, idx + 1, params))
            return true;
        params.erase(node.param_name);
    }

    return false;
}

const HandlerFn* Router::find_handler(const TrieNode& node,
                                       const std::string& method) const {
    auto it = node.handlers.find(method);
    return (it != node.handlers.end()) ? &it->second : nullptr;
}

} // namespace cortex::serving
