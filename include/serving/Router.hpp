#pragma once
// Router — trie-based HTTP route dispatcher.
//
// Maps {method, path_pattern} -> handler function.
// Supports path parameters via :param syntax (e.g. /stats/:gameId).

#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cortex::serving {

using HandlerFn = std::function<void(Request&, Response&, ServerContext&)>;

struct RouteMatch {
    HandlerFn handler;
    std::unordered_map<std::string, std::string> params;
};

class Router {
public:
    Router() = default;

    // Register a route handler.
    // pattern uses :param syntax for path parameters, e.g. "/stats/:gameId".
    void add(const std::string& method,
             const std::string& pattern,
             HandlerFn handler);

    // Match a method + path (without query string).
    // Returns the handler and extracted path params, or nullopt if no match.
    std::optional<RouteMatch> match(const std::string& method,
                                     const std::string& path) const;

private:
    struct TrieNode {
        // Literal children: segment string -> child node
        std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;

        // Parameter child (at most one per node): :paramName
        std::unique_ptr<TrieNode> param_child;
        std::string               param_name;   // e.g. "gameId"

        // Handlers for this node, keyed by HTTP method
        std::unordered_map<std::string, HandlerFn> handlers;
    };

    TrieNode root_;

    // Split a path "/a/b/c" into ["a", "b", "c"].
    static std::vector<std::string> split_path(const std::string& path);

    // Recursive match helper.
    bool match_impl(const TrieNode& node,
                    const std::vector<std::string>& segments,
                    size_t idx,
                    std::unordered_map<std::string, std::string>& params) const;

    // Find the handler at the matched node for the given method.
    const HandlerFn* find_handler(const TrieNode& node,
                                   const std::string& method) const;
};

} // namespace cortex::serving
