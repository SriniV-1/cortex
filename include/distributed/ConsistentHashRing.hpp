#pragma once
// ConsistentHashRing — deterministic game-to-worker assignment with minimal
// disruption on node join/leave.
//
// Uses virtual nodes (default 150 per physical node) mapped onto a 64-bit
// hash ring. Game IDs are hashed and assigned to the next clockwise node.
// Adding or removing a node migrates only ~1/N keys on average.
//
// Thread safety: NOT thread-safe. Caller must hold a mutex if accessed from
// multiple threads (Coordinator holds mu_ around all ring operations).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cortex::distributed {

class ConsistentHashRing {
public:
    explicit ConsistentHashRing(int vnodes_per_node = 150);

    // Add a physical node. Returns game_ids that migrate TO this node.
    std::unordered_set<std::string> add_node(const std::string& worker_id);

    // Remove a physical node. Returns game_id -> new owner for reassignment.
    std::unordered_map<std::string, std::string> remove_node(const std::string& worker_id);

    // Lookup: game_id -> worker_id. Returns nullopt if ring is empty.
    std::optional<std::string> assign(const std::string& game_id) const;

    // Track/untrack game IDs for migration computation.
    void track_game(const std::string& game_id);
    void untrack_game(const std::string& game_id);

    size_t node_count() const noexcept { return node_positions_.size(); }
    bool   empty() const noexcept { return ring_.empty(); }
    std::vector<std::string> nodes() const;

private:
    int vnodes_per_node_;

    // Hash position -> worker_id (sorted by position for clockwise walk).
    std::map<uint64_t, std::string> ring_;

    // worker_id -> its ring positions (for efficient removal).
    std::unordered_map<std::string, std::vector<uint64_t>> node_positions_;

    // Currently tracked game IDs.
    std::unordered_set<std::string> games_;

    // FNV-1a 64-bit hash. Deterministic, fast, good distribution.
    static uint64_t hash_key(const std::string& key, int vnode_index);

    // Find owner for a hash position (clockwise walk, wrapping).
    std::optional<std::string> find_owner(uint64_t hash) const;
};

} // namespace cortex::distributed
