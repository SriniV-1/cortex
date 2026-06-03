#pragma once
// TraceContext — lightweight UUID v4 trace ID generator.

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace cortex {

struct TraceContext {
    std::string trace_id;

    // Generate a new TraceContext with a random UUID v4 trace ID.
    // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx (RFC 4122 v4)
    static TraceContext create() {
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<uint32_t> dist(0, 15);

        // Hex digit lookup
        static constexpr char hex[] = "0123456789abcdef";
        // Variant nibble must be one of {8, 9, a, b}
        static constexpr char variant[] = "89ab";
        std::uniform_int_distribution<uint32_t> var_dist(0, 3);

        // UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
        //          8      - 4  - 4  - 4  - 12   = 32 hex + 4 dashes = 36 chars
        std::string uuid;
        uuid.reserve(36);

        auto append_hex = [&](int count) {
            for (int i = 0; i < count; ++i)
                uuid += hex[dist(rng)];
        };

        append_hex(8);          // xxxxxxxx
        uuid += '-';
        append_hex(4);          // xxxx
        uuid += '-';
        uuid += '4';            // version nibble
        append_hex(3);          // xxx
        uuid += '-';
        uuid += variant[var_dist(rng)];  // variant nibble (8, 9, a, or b)
        append_hex(3);          // xxx
        uuid += '-';
        append_hex(12);         // xxxxxxxxxxxx

        return TraceContext{std::move(uuid)};
    }
};

} // namespace cortex
