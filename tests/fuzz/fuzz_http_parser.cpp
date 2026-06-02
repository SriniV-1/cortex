// Phase 9.2 — Fuzz harness for HTTP request parsing via llhttp.
//
// Feeds arbitrary bytes into llhttp_execute to find crashes, hangs,
// or undefined behavior in the HTTP parser path.
//
// Build: clang++ -fsanitize=fuzzer,address -std=c++20 ...

#include <llhttp.h>

#include <cstdint>
#include <cstdlib>
#include <string>

// ── llhttp callbacks (minimal — just accumulate, don't crash) ────────────

static std::string g_url, g_header_field, g_header_value, g_body;

static int on_url(llhttp_t*, const char* at, size_t len) {
    if (g_url.size() < 4096) g_url.append(at, len);
    return 0;
}
static int on_header_field(llhttp_t*, const char* at, size_t len) {
    if (g_header_field.size() < 4096) g_header_field.append(at, len);
    return 0;
}
static int on_header_value(llhttp_t*, const char* at, size_t len) {
    if (g_header_value.size() < 4096) g_header_value.append(at, len);
    return 0;
}
static int on_headers_complete(llhttp_t*) {
    g_header_field.clear();
    g_header_value.clear();
    return 0;
}
static int on_body(llhttp_t*, const char* at, size_t len) {
    if (g_body.size() < 65536) g_body.append(at, len);
    return 0;
}
static int on_message_complete(llhttp_t*) {
    g_url.clear();
    g_header_field.clear();
    g_header_value.clear();
    g_body.clear();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input size to avoid excessive runtime per case
    if (size > 65536) return 0;

    g_url.clear();
    g_header_field.clear();
    g_header_value.clear();
    g_body.clear();

    llhttp_t parser;
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);

    settings.on_url              = on_url;
    settings.on_header_field     = on_header_field;
    settings.on_header_value     = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body             = on_body;
    settings.on_message_complete = on_message_complete;

    // Test as HTTP request
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    llhttp_execute(&parser, reinterpret_cast<const char*>(data), size);

    // Reset and also try as HTTP response
    g_url.clear(); g_body.clear();
    llhttp_init(&parser, HTTP_RESPONSE, &settings);
    llhttp_execute(&parser, reinterpret_cast<const char*>(data), size);

    return 0;
}
