#include "distributed/ConsistentHashRing.hpp"

#include <algorithm>
#include <cstring>

namespace cortex::distributed {

ConsistentHashRing::ConsistentHashRing(int vnodes_per_node)
    : vnodes_per_node_(vnodes_per_node) {}

// ── FNV-1a 64-bit hash ──────────────────────────────────────────────────────

uint64_t ConsistentHashRing::hash_key(const std::string& key, int vnode_index) {
    // Combine key with vnode index for distinct ring positions per virtual node.
    std::string combined = key + "#" + std::to_string(vnode_index);

    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

    uint64_t hash = FNV_OFFSET;
    for (char c : combined) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= FNV_PRIME;
    }
    return hash;
}

// ── Clockwise walk ───────────────────────────────────────────────────────────

std::optional<std::string> ConsistentHashRing::find_owner(uint64_t hash) const {
    if (ring_.empty()) return std::nullopt;

    // upper_bound gives us the first position strictly greater than hash.
    auto it = ring_.upper_bound(hash);
    if (it == ring_.end()) {
        // Wrap around to the first node.
        it = ring_.begin();
    }
    return it->second;
}

// ── Public API ───────────────────────────────────────────────────────────────

std::optional<std::string> ConsistentHashRing::assign(const std::string& game_id) const {
    if (ring_.empty()) return std::nullopt;
    uint64_t h = hash_key(game_id, 0);
    return find_owner(h);
}

std::unordered_set<std::string> ConsistentHashRing::add_node(const std::string& worker_id) {
    // Don't allow duplicate registration.
    if (node_positions_.count(worker_id)) return {};

    // Compute which games are currently assigned (before adding the node).
    std::unordered_map<std::string, std::string> old_assignments;
    for (const auto& gid : games_) {
        auto owner = assign(gid);
        if (owner) old_assignments[gid] = *owner;
    }

    // Insert virtual nodes.
    std::vector<uint64_t> positions;
    positions.reserve(vnodes_per_node_);
    for (int i = 0; i < vnodes_per_node_; ++i) {
        uint64_t pos = hash_key(worker_id, i);
        ring_[pos] = worker_id;
        positions.push_back(pos);
    }
    node_positions_[worker_id] = std::move(positions);

    // Determine which games migrated to the new node.
    std::unordered_set<std::string> migrated;
    for (const auto& gid : games_) {
        auto new_owner = assign(gid);
        if (new_owner && *new_owner == worker_id) {
            // This game is now assigned to the new node.
            auto it = old_assignments.find(gid);
            if (it == old_assignments.end() || it->second != worker_id) {
                migrated.insert(gid);
            }
        }
    }
    return migrated;
}

std::unordered_map<std::string, std::string>
ConsistentHashRing::remove_node(const std::string& worker_id) {
    auto it = node_positions_.find(worker_id);
    if (it == node_positions_.end()) return {};

    // Determine which games are currently assigned to this node.
    std::vector<std::string> affected;
    for (const auto& gid : games_) {
        auto owner = assign(gid);
        if (owner && *owner == worker_id) {
            affected.push_back(gid);
        }
    }

    // Remove all virtual nodes for this worker.
    for (uint64_t pos : it->second) {
        ring_.erase(pos);
    }
    node_positions_.erase(it);

    // Find new owners for affected games.
    std::unordered_map<std::string, std::string> reassignments;
    for (const auto& gid : affected) {
        auto new_owner = assign(gid);
        if (new_owner) {
            reassignments[gid] = *new_owner;
        }
    }
    return reassignments;
}

void ConsistentHashRing::track_game(const std::string& game_id) {
    games_.insert(game_id);
}

void ConsistentHashRing::untrack_game(const std::string& game_id) {
    games_.erase(game_id);
}

std::vector<std::string> ConsistentHashRing::nodes() const {
    std::vector<std::string> result;
    result.reserve(node_positions_.size());
    for (const auto& [id, _] : node_positions_) {
        result.push_back(id);
    }
    return result;
}

} // namespace cortex::distributed
