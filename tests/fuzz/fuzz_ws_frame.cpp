// Phase 9.2 — Fuzz harness for WebSocket frame parser.
//
// This is a standalone reimplementation of the frame-parsing logic from
// Connection::ws_parse_incoming() (src/serving/Connection.cpp lines 316-355).
// We feed arbitrary bytes and verify no out-of-bounds reads, infinite loops,
// or integer overflows occur.
//
// Build: clang++ -fsanitize=fuzzer,address -std=c++20 ...

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Standalone WebSocket frame parser — mirrors Connection::ws_parse_incoming().
// Returns the number of complete frames parsed.
static int ws_parse_frames(const uint8_t* data, size_t size) {
    std::string read_buf(reinterpret_cast<const char*>(data), size);
    int frames_parsed = 0;

    while (read_buf.size() >= 2) {
        uint8_t b0 = static_cast<uint8_t>(read_buf[0]);
        uint8_t b1 = static_cast<uint8_t>(read_buf[1]);
        bool    masked      = (b1 & 0x80) != 0;
        uint8_t opcode      = b0 & 0x0F;
        size_t  payload_len = b1 & 0x7F;
        size_t  header_len  = 2 + (masked ? 4 : 0);

        if (payload_len == 126) {
            if (read_buf.size() < 4) break;
            payload_len  = (static_cast<uint8_t>(read_buf[2]) << 8)
                         |  static_cast<uint8_t>(read_buf[3]);
            header_len  += 2;
        } else if (payload_len == 127) {
            if (read_buf.size() < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8) | static_cast<uint8_t>(read_buf[2 + i]);
            header_len += 8;
        }

        // Guard against absurdly large payloads that would OOM
        if (payload_len > 16 * 1024 * 1024) break;

        if (read_buf.size() < header_len + payload_len) break;

        // Extract and unmask payload (same as Connection code)
        std::string payload(read_buf.data() + header_len, payload_len);
        if (masked) {
            const char* mask = read_buf.data() + header_len - 4;
            for (size_t i = 0; i < payload_len; ++i)
                payload[i] ^= mask[i % 4];
        }

        read_buf.erase(0, header_len + payload_len);
        ++frames_parsed;

        // Handle close frame — stop parsing (mirrors original behavior)
        if (opcode == 0x8) break;

        // Limit iterations to prevent pathological inputs
        if (frames_parsed > 1000) break;
    }

    return frames_parsed;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input to avoid excessive runtime
    if (size > 65536) return 0;
    ws_parse_frames(data, size);
    return 0;
}
