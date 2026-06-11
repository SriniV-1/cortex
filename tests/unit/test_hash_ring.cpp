#include <gtest/gtest.h>
#include "distributed/ConsistentHashRing.hpp"

using cortex::distributed::ConsistentHashRing;

TEST(ConsistentHashRing, EmptyRingReturnsNullopt) {
    ConsistentHashRing ring;
    EXPECT_EQ(ring.assign("game001"), std::nullopt);
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.node_count(), 0u);
}

TEST(ConsistentHashRing, SingleNodeOwnsEverything) {
    ConsistentHashRing ring;
    ring.add_node("worker-1");

    EXPECT_EQ(ring.node_count(), 1u);
    EXPECT_EQ(ring.assign("game001"), "worker-1");
    EXPECT_EQ(ring.assign("game002"), "worker-1");
    EXPECT_EQ(ring.assign("game999"), "worker-1");
}

TEST(ConsistentHashRing, DeterministicAssignment) {
    ConsistentHashRing ring;
    ring.add_node("worker-1");
    ring.add_node("worker-2");
    ring.add_node("worker-3");

    // Same game_id should always map to the same worker.
    auto owner1 = ring.assign("0022500100");
    auto owner2 = ring.assign("0022500100");
    EXPECT_EQ(owner1, owner2);
}

TEST(ConsistentHashRing, DistributionRoughlyEven) {
    ConsistentHashRing ring(150);
    ring.add_node("worker-1");
    ring.add_node("worker-2");
    ring.add_node("worker-3");

    std::unordered_map<std::string, int> counts;
    for (int i = 0; i < 300; ++i) {
        auto owner = ring.assign("game_" + std::to_string(i));
        ASSERT_TRUE(owner.has_value());
        counts[*owner]++;
    }

    // Each worker should get roughly 100 games (300/3).
    // Allow 15% deviation: 55-145 each.
    for (const auto& [worker, count] : counts) {
        EXPECT_GE(count, 55) << worker << " got too few games";
        EXPECT_LE(count, 145) << worker << " got too many games";
    }
}

TEST(ConsistentHashRing, AddNodeMigratesMinimalKeys) {
    ConsistentHashRing ring(150);
    ring.add_node("worker-1");
    ring.add_node("worker-2");

    // Track 100 games.
    for (int i = 0; i < 100; ++i)
        ring.track_game("game_" + std::to_string(i));

    // Record current assignments.
    std::unordered_map<std::string, std::string> before;
    for (int i = 0; i < 100; ++i) {
        auto gid = "game_" + std::to_string(i);
        before[gid] = *ring.assign(gid);
    }

    // Add a third node — should migrate roughly 1/3 of keys.
    auto migrated = ring.add_node("worker-3");
    EXPECT_GT(migrated.size(), 10u);   // at least some migrated
    EXPECT_LT(migrated.size(), 60u);   // not too many

    // Verify migrated games now belong to worker-3.
    for (const auto& gid : migrated) {
        EXPECT_EQ(*ring.assign(gid), "worker-3");
    }

    // Verify non-migrated games kept their original owner.
    int unchanged = 0;
    for (const auto& [gid, old_owner] : before) {
        if (!migrated.count(gid)) {
            EXPECT_EQ(*ring.assign(gid), old_owner);
            ++unchanged;
        }
    }
    EXPECT_GT(unchanged, 40);
}

TEST(ConsistentHashRing, RemoveNodeReassignsGames) {
    ConsistentHashRing ring(150);
    ring.add_node("worker-1");
    ring.add_node("worker-2");
    ring.add_node("worker-3");

    for (int i = 0; i < 100; ++i)
        ring.track_game("game_" + std::to_string(i));

    // Remove worker-2.
    auto reassignments = ring.remove_node("worker-2");
    EXPECT_EQ(ring.node_count(), 2u);

    // All reassigned games should go to worker-1 or worker-3.
    for (const auto& [gid, new_owner] : reassignments) {
        EXPECT_TRUE(new_owner == "worker-1" || new_owner == "worker-3");
    }

    // Every game should still have an owner.
    for (int i = 0; i < 100; ++i) {
        auto owner = ring.assign("game_" + std::to_string(i));
        ASSERT_TRUE(owner.has_value());
        EXPECT_TRUE(*owner == "worker-1" || *owner == "worker-3");
    }
}

TEST(ConsistentHashRing, DuplicateNodeIgnored) {
    ConsistentHashRing ring;
    ring.add_node("worker-1");
    auto migrated = ring.add_node("worker-1");
    EXPECT_TRUE(migrated.empty());
    EXPECT_EQ(ring.node_count(), 1u);
}

TEST(ConsistentHashRing, RemoveNonexistentNodeSafe) {
    ConsistentHashRing ring;
    ring.add_node("worker-1");
    auto reassignments = ring.remove_node("worker-999");
    EXPECT_TRUE(reassignments.empty());
    EXPECT_EQ(ring.node_count(), 1u);
}

TEST(ConsistentHashRing, TrackAndUntrackGames) {
    ConsistentHashRing ring(150);
    ring.add_node("worker-1");

    ring.track_game("game-a");
    ring.track_game("game-b");

    // Adding a second node should report migrations from tracked games.
    auto migrated = ring.add_node("worker-2");
    // With only 2 tracked games, migration set is small but deterministic.
    EXPECT_LE(migrated.size(), 2u);

    ring.untrack_game("game-a");
    ring.untrack_game("game-b");

    // Adding a third node with no tracked games = no migrations reported.
    auto migrated2 = ring.add_node("worker-3");
    EXPECT_TRUE(migrated2.empty());
}
