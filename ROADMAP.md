# CORTEX — Real-Time NBA Analytics Engine
## C++20 · PostgreSQL 15 · Redis 7 · ONNXRuntime · ARM NEON SIMD

> **Agent handoff note:** Read this file first. It is the single source of truth for project state.
> Update the status table and "Current focus" section whenever a phase item completes or a blocker surfaces.

---

## Project Overview

Four-layer C++20 system that ingests NBA game data, computes real-time statistics,
and serves live analytics via HTTP/WebSocket. Achieved: sub-20ms p99 query latency
across 4.7M play-by-play events (8,400+ games, 1,250 players, 30 teams, 2019-2026).

**Platform:** macOS arm64 (Apple Silicon) for development.
- epoll in the spec -> replaced with **kqueue** on macOS (same semantics, BSD origin)
- AVX2 SIMD in the spec -> **ARM NEON** on M-series chips
- Linux support via `EpollPoller` behind the `IOPoller` interface — compile-time platform selection

**Working NBA data source (confirmed):**
`https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData/`
- `scoreboard/todaysScoreboard_00.json` — today's games
- `playbyplay/playbyplay_{gameId}.json` — play-by-play actions (~400-600 per game)
- `boxscore/boxscore_{gameId}.json` — box scores

---

## Status Overview

| Phase | Component | Status | Notes |
|-------|-----------|--------|-------|
| **Phase 1** | Dependencies installed | Done | cmake, psql 15, redis, libpqxx, spdlog, simdjson, gtest, onnxruntime |
| **Phase 1** | NBA API endpoint verified | Done | S3 endpoint works; stats.nba.com blocked |
| **Phase 1** | PostgreSQL schema | Done | Range-partitioned play_events, materialized views, team_elo table |
| **Phase 1** | C++ ETL pipeline | Done | NBAClient + BulkInserter + COPY protocol (~50K rows/sec) |
| **Phase 1** | Dimension tables populated | Done | fetch_boxscore() + --populate-dimensions backfill |
| **Phase 1** | Historical bulk load | Done | 8,434 games / 4.7M events / 30 teams / 1,250 players (2019-2026) |
| **Phase 1** | Playoff ETL with gap tolerance | Done | 80-miss threshold for inter-round ID gaps |
| **Phase 1** | Materialized view (player_game_stats) | Done | Concurrent refresh after ETL + daily refresh |
| **Phase 1** | Query benchmark (<20ms p99) | Done | game_events 3.2ms, player_season 6.3ms, game_summary 0.6ms |
| **Phase 2** | Lock-free ring buffer | Done | SPSC, power-of-2, cache-line padded — 8.7M ev/sec |
| **Phase 2** | Stream processor | Done | jthread consumer, dispatches to accumulator + callbacks |
| **Phase 2** | Rolling window aggregations | Done | StatAccumulator: last-N events per player x game |
| **Phase 2** | ONNX model integration | Done | WinProbModel: 7-feature logistic regression, 75.5% acc, 0.837 AUC |
| **Phase 2** | Memory management | Done | Time-based eviction (4hr stale), WS backpressure (1024 frame cap) |
| **Phase 3** | kqueue I/O event loop | Done | KqueuePoller: edge-triggered, pipe wakeup, 50ms timeout |
| **Phase 3** | HTTP/WebSocket server | Done | llhttp parsing, WebSocket RFC 6455, 12 REST + WS endpoints |
| **Phase 3** | Prometheus metrics | Done | GET /metrics — cortex_events_processed counter |
| **Phase 3** | Redis caching layer | Done | RedisCache: cache-aside on /stats, 60s TTL, graceful degradation |
| **Phase 3** | Load test: 1000 WS clients | Done | p99 = 15.6ms (target: <20ms) |
| **Phase 3** | Graceful shutdown | Done | Interruptible sleep, explicit jthread join ordering, no segfault |
| **Phase 4** | SIMD similarity search | Done | ARM NEON brute-force L2 scan, 4.7M vectors, ~6ms p99 |
| **Phase 4** | Elo rating system | Done | K=20/32, home advantage, season regression, built in 47ms |
| **Phase 4** | Win prob model retrained with Elo | Done | 7 features (added elo_diff, elo_expected), 0.837 AUC |
| **Phase 4** | Search endpoints | Done | Player search, game/team search, event search (whitelist validated) |
| **Phase 4** | Interactive dashboard | Done | Leaderboard tabs, game type filter, team autofill, Elo modal, live WS auto-connect |
| **Phase 4** | Daily auto-refresh | Done | 4 AM background thread, current season only, materialized view refresh |
| **Phase 5** | GitHub Actions CI | Done | Matrix build (Ubuntu + macOS), ctest, ASan/UBSan on every push |
| **Phase 5** | Docker containerization | Done | Multi-stage Dockerfile, docker-compose (Postgres + Redis + Cortex) |
| **Phase 5** | CMake portability | Done | Replace hardcoded /opt/homebrew paths with find_package/pkg_check_modules |
| **Phase 5** | Integration tests | Done | 6 end-to-end tests: schema, games, events, Elo determinism, API health, leaderboard |
| **Phase 5** | API rate limiting | Done | Token bucket rate limiter (50 req/sec, 100 burst), 429 on exceeded, per-IP tracking |
| **Phase 5** | Enriched Prometheus metrics | Done | 5 metric families: events, active games, rate limiter, similarity index, Elo |
| **Phase 5** | Grafana observability | Done | Pre-built dashboard JSON (8 panels) + Prometheus scrape config |
| **Phase 5** | Frontend data visualization | Done | Chart.js Elo trajectory (line) + rating distribution (bar), /api/elo/history endpoint |
| **Phase 6** | Router abstraction & HttpServer decomposition | Planned | Trie-based router, Request/Response types, handler files, ServerContext DI |
| **Phase 6** | Structured JSON serialization | Planned | nlohmann/json replaces ostringstream builders |
| **Phase 6** | Dependency injection & testability | Planned | ICache/IDatabase interfaces, mock-based handler tests |
| **Phase 7** | Connection pooling & async DB | Planned | Fixed-size pqxx pool, DB queries off event loop thread |
| **Phase 7** | Request tracing & structured logging | Planned | UUID trace_id per request, X-Trace-Id header, structured log lines |
| **Phase 7** | Cursor-based API pagination | Planned | Base64 cursors, default limit=50, has_more flag |
| **Phase 7** | JWT authentication & RBAC | Planned | Bearer token validation, viewer/admin roles, auth middleware |
| **Phase 8** | HNSW approximate nearest neighbor index | Planned | Hand-rolled HNSW graph, >95% recall@10, <1ms query at 4.7M vectors |
| **Phase 8** | Distributed stream processing cluster | Planned | Multi-node ingestors, coordinator game assignment, failure detection, state handoff |
| **Phase 8** | Consistent hashing & shard health routing | Planned | Hash ring with virtual nodes, heartbeat-based failure detection, automatic reassignment |
| **Phase 8** | AVX2/SSE4 SIMD path for x86 | Planned | Dual SIMD backend (NEON + AVX2), runtime dispatch |
| **Phase 9** | Property-based testing | Planned | RapidCheck: 10+ properties across 5 components |
| **Phase 9** | Fuzz testing | Planned | libFuzzer harnesses for HTTP parser, WS frames, JSON serializer |
| **Phase 9** | CI performance regression tests | Planned | Benchmark baselines, >20% regression fails build |
| **Phase 10** | Deep health checks & readiness probes | Planned | /health checks all deps, /ready for orchestrator probes |
| **Phase 10** | Circuit breaking & graceful degradation | Planned | 3-state circuit breaker on Redis + DB |
| **Phase 10** | Backpressure throughout pipeline | Planned | Load shedding, 503+Retry-After, dropped event counters |
| **Phase 11** | OpenAPI documentation | Planned | Full OpenAPI 3.0 spec, Swagger UI at /docs |
| **Phase 11** | Dashboard UX polish | Planned | Auto-refresh, loading states, dark mode, mobile responsive |

---

## Phase 1 — Data Layer

**Goal:** PostgreSQL schema, C++ ETL that bulk-loads historical NBA data, queries run <20ms p99.

**Result:**
- 8,434 games loaded (2019-2026 regular season + playoffs)
- 4,705,700 play-by-play events
- 30 teams, 1,250 players
- Range-partitioned across 6 time-based partitions (native PG15, no TimescaleDB needed)
- Materialized view `player_game_stats` for instant leaderboard queries
- All query benchmarks under 7ms p99

**Technical decisions:**
- **ETL language:** C++ with libcurl + libpqxx — uses PostgreSQL COPY protocol for bulk insert
- **Concurrency:** `std::jthread` pool (configurable, default 4 threads) for parallel game fetching
- **Rate limiting:** 100ms sleep between S3 requests
- **Partitioning:** Native PG15 range partitioning (TimescaleDB had Xcode toolchain issues)
- **Playoff ID gaps:** 80-miss early termination threshold (vs 30 for regular season) because playoff game IDs have natural gaps between rounds

---

## Phase 2 — Stream Processing

**Goal:** Lock-free ring buffer ingests live play-by-play events; stream processor
computes rolling window statistics and win probability.

**Result:**
- SPSC ring buffer: 8.7M events/sec throughput
- StatAccumulator: per-player/game atomic counters with rolling-window momentum
- WinProbModel: 7-feature ONNX logistic regression with Elo integration (0.837 AUC)
- Time-based memory eviction every 10 minutes (4-hour staleness threshold)
- WebSocket backpressure: 1024-frame queue cap per connection

**Technical decisions:**
- **Ring buffer:** SPSC `std::atomic` head/tail, cache-line padding (64 bytes)
- **Memory ordering:** `acquire`/`release` — no mutex in hot path
- **ONNX model:** Trained with scikit-learn on 42K samples, exported to ONNX (217 bytes)
- **Elo features:** `elo_diff` and `elo_expected` are the two strongest predictors (coef +1.34 for expected)

---

## Phase 3 — Serving Layer

**Goal:** HTTP/WebSocket server using kqueue/epoll I/O multiplexing. Serves live stats
via WebSocket, historical queries via REST.

**Result:**
- 12 REST API endpoints + WebSocket live streaming
- 1000 concurrent WebSocket clients at 15.6ms p99 broadcast latency
- Redis cache-aside with 60s TTL and graceful degradation
- Auto-subscribes to live games on page load
- Graceful shutdown: interruptible sleeps, explicit jthread join ordering

**Technical decisions:**
- **Platform abstraction:** `IOPoller` interface with `KqueuePoller` (macOS) and `EpollPoller` (Linux)
- **HTTP parsing:** llhttp (same parser as Node.js)
- **WebSocket:** Hand-rolled RFC 6455 frames (text only, no fragmentation)
- **Static file serving:** Serves `www/` directory with proper MIME types

---

## Phase 4 — Analytics & Intelligence

**Goal:** Advanced analytics features — similarity search, team ratings, enhanced
win probability, interactive dashboard.

**Result:**
- SIMD similarity search across 4.7M game-state vectors in ~6ms p99 (142 MB feature store)
- Elo rating system: K=20 regular, K=32 playoffs, 100-point home court advantage, 25% season regression
- Win probability model retrained with Elo features (7 inputs, 75.5% accuracy, 0.837 AUC)
- Full-featured dashboard: leaderboard with 6 stat tabs, player/game/event search, game type filtering, team autofill, Elo power rankings with expandable modal, live WebSocket auto-connect

---

## Phase 5 — Production Readiness & Polish

**Goal:** Close the gap between "impressive intern project" and "production-grade system" —
CI/CD automation, containerized deployment, cross-platform portability, integration testing,
and frontend data visualization.

**Planned work:**

1. **GitHub Actions CI** — Matrix build targeting Ubuntu 24.04 + macOS 14 (ARM).
   Runs `ctest --output-on-failure` on every push/PR. Debug builds with
   `-fsanitize=address,undefined` to catch regressions. Caches vcpkg/Homebrew
   deps for fast CI turnaround.

2. **Docker containerization** — Multi-stage Dockerfile: build stage compiles
   Cortex with all deps, runtime stage ships just the binary + ONNX model + www/.
   `docker-compose.yml` orchestrates PostgreSQL 15, Redis 7, and Cortex with
   health checks and proper startup ordering. One-command local setup:
   `docker compose up`.

3. **CMake portability** — Replace all hardcoded `/opt/homebrew` paths with
   `find_library()` / `pkg_check_modules()` that work on both macOS (Homebrew)
   and Linux (apt/vcpkg). Keep Apple-specific hints as fallbacks, not requirements.

4. **Integration tests** — Tests that spin up a test database with a small
   fixture dataset (~100 games), run ETL, then verify end-to-end correctness:
   API responses match expected JSON structure, leaderboard ordering is correct,
   Elo ratings are deterministic, similarity search returns sane results.

5. **API rate limiting** — Token bucket rate limiter (`RateLimiter.hpp`) with
   configurable burst (100) and refill rate (50 req/sec) per client IP.
   Applied to all endpoints except `/health` and `/metrics`. Returns HTTP 429
   on exceeded limits. Stale buckets evicted after 5 minutes.

6. **Enriched Prometheus metrics** — Expanded `/metrics` endpoint from 1 to 5
   metric families: `cortex_events_processed`, `cortex_active_games`,
   `cortex_rate_limiter_buckets`, `cortex_similarity_index_size`,
   `cortex_elo_games_processed`. Added `game_count()` to StatAccumulator.

7. **Grafana observability dashboard** — Pre-built Grafana JSON provisioning
   file (`grafana/cortex-dashboard.json`) with 8 panels: 4 stat panels
   (events, games, index size, Elo) + 4 time-series panels (throughput rate,
   active clients, games over time, cumulative events). Includes Prometheus
   scrape config (`grafana/prometheus.yml`).

8. **Frontend data visualization** — Chart.js integrated via CDN. Two new
   chart panels below the Elo rankings: (1) Elo Rating Trajectory — line chart
   showing per-team ratings across all seasons with Top 5/Top 10/All 30 toggle
   controls and team-specific colors; (2) Rating Distribution — horizontal bar
   chart of all 30 teams' current Elo ratings sorted descending. New backend
   endpoint `/api/elo/history` serves season-end snapshots captured during
   `build_from_db()` via `EloSnapshot` struct. Charts use team brand colors,
   custom tooltips with JetBrains Mono, and responsive sizing.

---

## Performance Summary

| Benchmark | Result |
|-----------|--------|
| Ring buffer throughput | 8.7M events/sec |
| Similarity search (4.7M vectors) | ~6ms p99 |
| Query latency (game events) | 3.2ms p99 |
| Query latency (player season) | 6.3ms p99 |
| WebSocket broadcast (1000 clients) | 15.6ms p99 |
| Win probability inference | ~0.1ms |
| Elo build (8,400+ games) | 47ms |

---

## Codebase Statistics

| Component | Lines | Files |
|-----------|-------|-------|
| C++ source + headers | 5,479 | 24 |
| Unit tests + benchmarks | 1,625 | 8 |
| Frontend (HTML/CSS/JS) | ~1,100 | 1 |
| SQL schema | 228 | 1 |
| Integration tests | ~240 | 1 |
| Shell + Python scripts | ~615 | 3 |
| CMake build | 220 | 1 |
| CI/CD + Docker | ~160 | 4 |
| **Total** | **~9,800** | **44** |

---

## Key File Map

```
CORTEX/
├── ROADMAP.md              <- this file (keep updated)
├── README.md               <- user-facing docs
├── PROJECT_DESCRIPTION.md  <- resume/LinkedIn handoff
├── CMakeLists.txt          <- root build (7 executables + 4 static libraries)
├── cortex.sh               <- start/stop/load/bench convenience script
├── Dockerfile              <- multi-stage build (Phase 5)
├── docker-compose.yml      <- one-command local setup (Phase 5)
├── .github/
│   └── workflows/
│       └── ci.yml          <- GitHub Actions CI (Phase 5)
├── sql/
│   └── schema.sql          <- PG15 schema with range partitions + materialized views
├── include/
│   ├── common/             <- Logger, Config
│   ├── etl/                <- NBAClient, BulkInserter, LiveIngestor
│   ├── stream/             <- RingBuffer, StreamProcessor, StatAccumulator, StreamEvent
│   ├── serving/            <- IOPoller, KqueuePoller, EpollPoller, HttpServer, RedisCache
│   └── analytics/          <- WinProbModel, GameStateIndex, EloTracker
├── src/
│   ├── etl/                <- NBAClient.cpp, BulkInserter.cpp, LiveIngestor.cpp, main_etl.cpp
│   ├── stream/             <- StreamProcessor.cpp, StatAccumulator.cpp
│   ├── serving/            <- KqueuePoller.cpp, EpollPoller.cpp, HttpServer.cpp, RedisCache.cpp, main_server.cpp
│   └── analytics/          <- WinProbModel.cpp, GameStateIndex.cpp, EloTracker.cpp
├── scripts/
│   ├── bootstrap_db.sh     <- creates DB, runs schema.sql
│   └── train_win_prob.py   <- model training (scikit-learn -> ONNX export)
├── data/
│   └── models/
│       └── win_prob.onnx   <- 7-feature logistic regression (217 bytes)
├── tests/
│   ├── unit/               <- 34 gtest tests across 10 suites
│   └── benchmarks/         <- 4 benchmark executables
├── www/
│   └── index.html          <- single-page dashboard (~1,100 lines)
└── config/
    └── cortex.toml         <- runtime config (DB URL, Redis URL, thread counts)
```

---

## Environment

```bash
# PostgreSQL 15 (Homebrew)
PGHOST=localhost
PGPORT=5433
PGDATABASE=cortex
PGUSER=srini

# Redis 7 (Homebrew)
REDIS_URL=redis://localhost:6379

# NBA S3 base
NBA_S3_BASE=https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData
```

---

## Resolved Issues

| # | Issue | Resolution |
|---|-------|------------|
| 1 | TimescaleDB build failed (Xcode toolchain mismatch) | Using native PG15 range partitioning — functionally equivalent |
| 2 | stats.nba.com Akamai-blocked | Resolved via S3 endpoint |
| 3 | Historical game ID list not available from S3 | `game_ids_for_season()` generates IDs deterministically from NBA format |
| 4 | Playoff ETL missing conference finals / finals | Increased early-termination threshold to 80 for playoffs (inter-round ID gaps) |
| 5 | WebSocket never auto-connected to live games | `connectStream()` was defined but never called; added auto-subscribe on live game detection |
| 6 | Segfault on Ctrl+C shutdown | Background jthreads used non-interruptible `sleep_for()`; replaced with 1-sec tick loop checking `stop_requested()` |
| 7 | StatAccumulator maps grew monotonically | Added time-based eviction (4-hour staleness, 10-minute sweep interval) |
| 8 | WebSocket outbound queue unbounded | Capped at 1024 frames per connection; slow clients disconnected |
| 9 | `shared_mutex` deadlock in EloTracker | `build_from_db()` held unique_lock then called method needing shared_lock; inlined the logic |
| 10 | Live games never appeared in dashboard | `/api/games/recent` only queried DB (status=3). Added `/api/scoreboard` backed by LiveIngestor's cached scoreboard snapshot; frontend now fetches both endpoints in parallel |

---

## Phase 6 — Architectural Hardening

**Goal:** Eliminate the structural weaknesses that prevent this project from scoring above 7/10
in architecture and code quality reviews. These are foundational refactors — every later phase
depends on them being done first.

**Target scores:** Architecture 8.5→9, Code Quality 8→9, Resume Strength +0.5

### 6.1 — Router Abstraction & HttpServer Decomposition

**Problem:** `HttpServer.cpp` is ~800 lines of chained if-statements mixing routing, request
parsing, JSON serialization, DB queries, and WebSocket management. This is the single biggest
architectural red flag in the codebase.

**Work:**
1. Create `include/serving/Router.hpp` — A `Router` class that maps `{method, path_pattern}` →
   `std::function<void(Request&, Response&)>` handler. Support path parameters via `:param`
   syntax (e.g., `/stats/:gameId`). Use a trie or sorted vector, not nested if-chains.
2. Create `include/serving/Request.hpp` and `include/serving/Response.hpp` — Typed wrappers
   around the raw HTTP data. `Request` owns method, path, path params, query params, headers,
   body, client IP. `Response` owns status code, headers, body; has `json()`, `text()`,
   `send_file()` helpers.
3. Create `src/serving/Router.cpp` — Route matching with path parameter extraction.
4. Split route handlers into separate files under `src/serving/handlers/`:
   - `health_handler.cpp` — `/health`
   - `stats_handler.cpp` — `/stats/:gameId`, `/players/:playerId/season`
   - `games_handler.cpp` — `/api/games/recent`, `/api/scoreboard`
   - `search_handler.cpp` — `/api/search/players`, `/api/search/games`, `/api/search/events`
   - `analytics_handler.cpp` — `/api/elo/rankings`, `/api/elo/history`, `/api/similarity`
   - `metrics_handler.cpp` — `/metrics`
   - `ws_handler.cpp` — WebSocket upgrade + subscription logic
   - `static_handler.cpp` — Static file serving from `www/`
5. Refactor `HttpServer.cpp` down to ~200 lines: accept loop, connection lifecycle, route
   dispatch. All business logic lives in handlers.
6. Introduce `ServerContext` struct to replace the 10+ raw pointer injections on `Connection`:
   ```cpp
   struct ServerContext {
       StatAccumulator& accumulator;
       pqxx::connection& db;
       RedisCache& cache;
       const GameStateIndex& index;
       const EloTracker& elo;
       const LiveIngestor& ingestor;
       RateLimiter& limiter;
       std::string www_root;
   };
   ```

**Files to create:** `include/serving/Router.hpp`, `include/serving/Request.hpp`,
`include/serving/Response.hpp`, `src/serving/Router.cpp`,
`src/serving/handlers/{health,stats,games,search,analytics,metrics,ws,static}_handler.cpp`

**Files to modify:** `include/serving/HttpServer.hpp`, `src/serving/HttpServer.cpp`,
`CMakeLists.txt`

**Tests:** Add `tests/unit/test_router.cpp` — route registration, path param extraction,
404 on unmatched, method filtering. Existing `test_serving.cpp` must still pass.

**Acceptance criteria:**
- `HttpServer.cpp` is under 250 lines
- No if-chain routing remains
- Path parameters work: `/stats/0022300123` extracts `gameId=0022300123`
- All 12 existing endpoints return identical responses
- All existing tests pass unchanged

---

### 6.2 — Structured JSON Serialization

**Problem:** All JSON responses are built via `std::ostringstream` with manual string
concatenation and comma tracking. This is error-prone, unescaped, and won't survive a
code review at any top company.

**Work:**
1. Add `nlohmann/json` as a dependency (header-only, add via CMake FetchContent or vendored
   single-include). Do NOT use simdjson for serialization — it is parse-only.
2. Replace every `std::ostringstream` JSON builder in the handler files (created in 6.1) with
   `nlohmann::json` objects. Use structured construction:
   ```cpp
   json j = {{"game_id", game_id}, {"home_score", home}, {"away_score", away}};
   response.json(j.dump());
   ```
3. Keep simdjson for *parsing* inbound NBA API responses (ETL layer). Use nlohmann for
   *serializing* outbound API responses (serving layer).

**Files to modify:** All handler files from 6.1, `CMakeLists.txt`

**Tests:** Verify all API responses are valid JSON (parse with nlohmann in tests).

**Acceptance criteria:**
- Zero `std::ostringstream` usage for JSON construction in serving layer
- All API responses parse as valid JSON
- Special characters in player names are properly escaped
- No performance regression (nlohmann is fast enough for response serialization)

---

### 6.3 — Dependency Injection & Testability

**Problem:** Components are wired together via raw pointers assigned after construction.
This makes unit testing difficult and ownership unclear.

**Work:**
1. Define `ServerContext` (from 6.1) as the canonical dependency container. Passed by
   reference to all handler functions.
2. Extract interfaces for key dependencies to enable mocking in tests:
   - `ICache` interface (RedisCache implements) — `get()`, `set()`, `del()`
   - `IDatabase` interface — wraps pqxx query execution
3. Handlers accept `ServerContext&` — no raw pointer juggling.
4. Write at least 3 handler unit tests using mock cache + mock DB.

**Files to create:** `include/serving/ICache.hpp`, `include/serving/IDatabase.hpp`

**Files to modify:** `include/serving/RedisCache.hpp`, handler files

**Tests:** `tests/unit/test_handlers.cpp` — unit tests for stats_handler and
analytics_handler with injected mocks.

**Acceptance criteria:**
- No raw pointer dependency injection on Connection
- Handler functions are independently testable
- At least 3 handler tests with mock dependencies pass

---

## Phase 7 — Production-Grade Infrastructure

**Goal:** Close the gap between "demo project" and "would survive a production incident."
These are the features a Stripe/Databricks reviewer would look for to validate production
thinking.

**Target scores:** Production Readiness 5→8.5, Engineering Complexity +1

### 7.1 — Connection Pooling & Async DB

**Problem:** Single `pqxx::connection` shared across all requests. No pooling. DB queries
execute synchronously on the I/O event loop thread, blocking all other connections.

**Work:**
1. Create `include/serving/ConnectionPool.hpp` — A fixed-size pool of `pqxx::connection`
   objects with checkout/return semantics. Use `std::counting_semaphore` or condition variable
   for blocking checkout when pool exhausted. Configurable pool size (default: 4).
2. Create `src/serving/ConnectionPool.cpp` — Implementation with connection health checking
   (ping on checkout, reconnect on failure).
3. Move DB query execution off the event loop thread: use a small thread pool (2-4 threads)
   for DB work. Handler posts query to thread pool, thread pool executes and writes response
   back via pipe wakeup to kqueue.
4. Update all handlers to use `pool.checkout()` instead of raw `db_conn` pointer.

**Files to create:** `include/serving/ConnectionPool.hpp`, `src/serving/ConnectionPool.cpp`

**Files to modify:** `HttpServer.cpp`, all handler files, `CMakeLists.txt`

**Tests:** `tests/unit/test_connection_pool.cpp` — concurrent checkout/return, pool
exhaustion blocking, reconnect on stale connection.

**Acceptance criteria:**
- DB queries no longer block the kqueue event loop
- Pool supports configurable size (2-8 connections)
- Connection health-checked on checkout
- Load test shows no degradation at 1000 concurrent clients

---

### 7.2 — Request Tracing & Structured Logging

**Problem:** Logs use `spdlog` but are unstructured — no correlation between request and
response logs. No trace ID propagation. Impossible to debug a specific request path.

**Work:**
1. Create `include/common/TraceContext.hpp` — Generates a UUID v4 trace_id per request.
   Propagated via `Request` object to all handler code and DB queries.
2. Modify logger to support structured key-value pairs: `log->info("query complete",
   {{"trace_id", ctx.trace_id}, {"duration_ms", elapsed}, {"rows", count}})`.
   Use spdlog's custom formatter or switch to structured JSON log lines.
3. Add `X-Trace-Id` response header on every HTTP response.
4. Add request logging middleware: log method, path, status, duration_ms, trace_id on
   every request completion.

**Files to create:** `include/common/TraceContext.hpp`

**Files to modify:** `include/common/Logger.hpp`, `Request.hpp`, `Response.hpp`, handler files

**Tests:** Verify trace_id appears in response headers and log output.

**Acceptance criteria:**
- Every request gets a unique trace_id
- Response includes `X-Trace-Id` header
- Logs include trace_id for correlation
- Request duration logged on completion

---

### 7.3 — API Pagination (Cursor-Based)

**Problem:** All list endpoints return unbounded result sets. `/api/games/recent` returns
all games. No pagination.

**Work:**
1. Add cursor-based pagination to list endpoints: `/api/games/recent`, `/api/search/*`,
   `/api/elo/rankings`.
2. Response format:
   ```json
   {
     "data": [...],
     "next_cursor": "eyJnYW1lX2lkIjoiMDAyMjMwMDEyMyJ9",
     "has_more": true
   }
   ```
3. Cursor is a base64-encoded JSON object containing the sort key of the last item.
4. Default page size: 50. Max page size: 200. Configurable via `?limit=N` query param.
5. Add `?cursor=<token>` query param support to the router.

**Files to modify:** Relevant handler files, `Router.hpp` (query param parsing)

**Tests:** Pagination tests — first page, next page via cursor, empty last page,
invalid cursor returns 400.

**Acceptance criteria:**
- All list endpoints paginated with default limit=50
- Cursor-based (not offset-based) for stable pagination
- `has_more` flag accurate
- Invalid/expired cursor returns 400 with descriptive error

---

### 7.4 — JWT Authentication & RBAC

**Problem:** All endpoints are unauthenticated. Any client can access any data. No concept
of users or roles.

**Work:**
1. Add a lightweight JWT library (e.g., `jwt-cpp`, header-only).
2. Create `include/serving/AuthMiddleware.hpp` — Validates `Authorization: Bearer <token>`
   header. Extracts claims (sub, role, exp). Rejects expired tokens.
3. Define two roles: `viewer` (read-only access to all GET endpoints) and `admin`
   (can trigger ETL refresh, clear cache, etc.).
4. Add `POST /api/auth/token` endpoint that issues JWTs (for demo purposes, accepts
   a hardcoded API key from config — NOT a full user management system).
5. Exempt from auth: `/health`, `/metrics`, `GET /` (dashboard), static files.
6. Add auth middleware to the router — runs before handler dispatch.

**Files to create:** `include/serving/AuthMiddleware.hpp`, `src/serving/AuthMiddleware.cpp`

**Files to modify:** `Router.hpp`, `HttpServer.cpp`, `CMakeLists.txt`, `config/cortex.toml`

**Tests:** Auth tests — valid token accepted, expired token rejected, missing token
rejected, role-based access (admin can POST, viewer cannot).

**Acceptance criteria:**
- All data endpoints require valid JWT (except exempted paths)
- Expired tokens return 401
- Role mismatch returns 403
- Token issuance endpoint works with configured API key
- Dashboard static files remain accessible without auth

---

## Phase 8 — Distributed Stream Processing & Advanced Analytics

**Goal:** Transform Cortex from a monolithic application into a genuinely distributed system.
The distribution is *load-bearing* — a single node cannot handle hundreds of concurrent live
games, so multiple ingestor nodes must coordinate game ownership, detect failures, and hand
off state. This is how Kafka consumer groups, Flink task managers, and Spark executors work.

This is what gets attention from Databricks, Confluent, Palantir, and infrastructure teams.
Not "I added gRPC" — but "I built a distributed stream processor with failure recovery."

**Target scores:** Differentiation 7→9.5, Technical Difficulty 8→9.5, Architecture 8.5→9.5

### 8.1 — HNSW Approximate Nearest Neighbor Index

**Problem:** Similarity search uses brute-force L2 scan over 4.7M vectors. While the SIMD
implementation is fast (~6ms), it doesn't demonstrate knowledge of production-grade ANN
techniques. Every ML infra team uses HNSW/IVF.

**Work:**
1. Create `include/analytics/HNSWIndex.hpp` — Hand-rolled HNSW (Hierarchical Navigable
   Small World) graph index. Key structures:
   - Multi-layer graph with skip-list-like hierarchy
   - Configurable M (max connections per layer, default 16) and efConstruction (default 200)
   - Uses same ARM NEON L2 distance function from GameStateIndex
2. Create `src/analytics/HNSWIndex.cpp` — Implementation:
   - `insert(id, vector)` — greedy search from top layer, connect to M nearest at each level
   - `search(query, k, ef)` — beam search with ef candidates, return top-k
   - `build(vectors)` — batch construction with progress logging
3. Integrate into `GameStateIndex` as an alternative backend:
   - Config flag: `similarity_backend = "brute_force" | "hnsw"`
   - Both backends implement same interface, results should be near-identical
   - Benchmark both: HNSW should be 10-100x faster for top-k queries
4. Add `/api/similarity?backend=hnsw` query param for A/B comparison.

**Files to create:** `include/analytics/HNSWIndex.hpp`, `src/analytics/HNSWIndex.cpp`

**Files to modify:** `include/analytics/GameStateIndex.hpp`, `src/analytics/GameStateIndex.cpp`,
search handler, `CMakeLists.txt`

**Tests:** `tests/unit/test_hnsw.cpp` — insertion, search recall (>95% overlap with
brute-force top-10), empty index, single element.
`tests/benchmarks/bench_hnsw.cpp` — throughput comparison vs brute-force at 100K, 1M, 4.7M
vectors.

**Acceptance criteria:**
- HNSW search returns >95% recall@10 vs brute-force ground truth
- Query latency under 1ms for top-10 at 4.7M vectors
- Memory overhead documented (expect ~2-3x raw vector storage)
- Benchmark shows speedup factor in CI output

---

### 8.2 — Distributed Stream Processing Cluster

**Problem:** The entire system runs in a single process. `LiveIngestor` polls all live games
on one thread. `StatAccumulator` holds all state in one process. There is no distributed
component — and bolting on a gRPC fan-out for reads doesn't make it distributed. The
distribution needs to be *essential*: multiple nodes must coordinate mutable state, and the
system must handle node failure without data loss.

**Design:** A coordinator-worker architecture modeled after Kafka consumer groups:
- **Coordinator** — single leader process that tracks cluster membership, assigns game
  ownership to workers, detects failures, and triggers reassignment
- **Workers** (IngestorNode) — each owns a subset of live games, runs its own
  `LiveIngestor` + `StatAccumulator` + `RingBuffer` for those games, and serves real-time
  stats for its assigned games
- The system *cannot function correctly with only a coordinator* — workers are required to
  do the actual ingestion. Distribution is load-bearing.

**Work:**

1. **Define the gRPC control plane** — `proto/cortex.proto`:
   ```protobuf
   service CoordinatorService {
     // Workers call this on startup to join the cluster
     rpc Register(RegisterRequest) returns (RegisterResponse);
     // Workers send periodic heartbeats (every 2s)
     rpc Heartbeat(HeartbeatRequest) returns (HeartbeatResponse);
     // Coordinator pushes new game assignments to workers
     rpc GetAssignment(AssignmentRequest) returns (stream AssignmentUpdate);
     // Workers report per-game stats snapshots back to coordinator for aggregation
     rpc ReportStats(StatsReport) returns (Ack);
   }

   message RegisterRequest {
     string worker_id = 1;    // UUID generated on startup
     string host = 2;         // worker's reachable address
     int32 port = 3;          // worker's HTTP port for stat queries
     int32 capacity = 4;      // max games this worker can handle
   }

   message HeartbeatRequest {
     string worker_id = 1;
     repeated string active_games = 2;    // games currently being ingested
     int64 events_processed = 3;          // total events since last heartbeat
     double cpu_load = 4;                 // for load-aware assignment
   }

   message AssignmentUpdate {
     enum Action { ASSIGN = 0; REVOKE = 1; }
     Action action = 1;
     string game_id = 2;
     int64 epoch = 3;         // fencing token — reject stale assignments
   }
   ```

2. **Create `include/distributed/Coordinator.hpp`** — The cluster brain:
   - Maintains a `WorkerRegistry` — map of worker_id → {host, port, capacity,
     last_heartbeat, assigned_games, epoch}
   - Polls the NBA scoreboard to discover live games (reuses `NBAClient`)
   - Assigns games to workers using least-loaded strategy
   - Detects worker failure: if no heartbeat received within 6 seconds (3 missed),
     marks worker as dead and reassigns its games to surviving workers
   - **Epoch-based fencing:** Every assignment carries a monotonically increasing epoch
     number. Workers reject operations for games whose epoch is stale. This prevents
     split-brain: if a "dead" worker comes back, its old epoch is invalid.
   - Aggregates stats reports from all workers for the `/api/stats` HTTP endpoints

3. **Create `src/distributed/Coordinator.cpp`** — Implementation:
   - gRPC server thread for Register/Heartbeat/ReportStats RPCs
   - Assignment loop thread (runs every 5 seconds):
     1. Fetch scoreboard → get list of live game IDs
     2. Diff against currently assigned games
     3. New games → assign to least-loaded worker via `GetAssignment` stream
     4. Finished games (status=3) → revoke from worker
   - Failure detection thread:
     1. Scan worker registry every 2 seconds
     2. Workers with `last_heartbeat > 6s ago` → mark dead
     3. Reassign dead worker's games to survivors with new epoch
     4. Log: "Worker {id} failed — reassigning {N} games to {survivors}"
   - HTTP API proxying: coordinator also runs HttpServer and proxies `/stats/{gameId}`
     to the worker that owns that game (via gRPC or direct HTTP forward)

4. **Create `include/distributed/IngestorNode.hpp`** — A worker node:
   - On startup: generates UUID worker_id, calls `Register()` on coordinator
   - Heartbeat thread: sends `Heartbeat()` every 2 seconds with active game list
   - Assignment stream: listens on `GetAssignment()` for ASSIGN/REVOKE updates
   - On ASSIGN(game_id, epoch):
     1. Validate epoch > last known epoch for this game (fencing)
     2. Start polling play-by-play for game_id via `NBAClient`
     3. Create per-game `StatAccumulator` entries
     4. Push events to local `RingBuffer` → `StreamProcessor`
   - On REVOKE(game_id, epoch):
     1. Stop polling for game_id
     2. Snapshot current `StatAccumulator` state for that game
     3. Send final stats report to coordinator before releasing

5. **Create `src/distributed/IngestorNode.cpp`** — Implementation.

6. **State handoff on reassignment:**
   When a game is reassigned (original worker died), the new worker:
   - Fetches full play-by-play from NBA S3 for the game
   - Replays events from the beginning (stateless recovery — ~500 events, <1 second)
   - Resumes from current watermark
   - This is simpler than state transfer between nodes and equally correct since
     `StatAccumulator` is deterministic

7. **CLI modes for `main_server`:**
   - `--mode=standalone` (default) — current single-process behavior, unchanged
   - `--mode=coordinator --grpc-port=50051` — runs coordinator + HTTP API
   - `--mode=worker --coordinator=host:50051 --http-port=8081 --capacity=20`

8. **Docker Compose multi-node deployment:**
   ```yaml
   coordinator:
     command: ["./cortex_server", "--mode=coordinator", "--grpc-port=50051"]
     ports: ["8080:8080", "50051:50051"]
   worker-1:
     command: ["./cortex_server", "--mode=worker", "--coordinator=coordinator:50051",
               "--http-port=8081", "--capacity=20"]
   worker-2:
     command: ["./cortex_server", "--mode=worker", "--coordinator=coordinator:50051",
               "--http-port=8082", "--capacity=20"]
   worker-3:
     command: ["./cortex_server", "--mode=worker", "--coordinator=coordinator:50051",
               "--http-port=8083", "--capacity=20"]
   ```

**Files to create:**
- `proto/cortex.proto` — gRPC service + message definitions
- `include/distributed/Coordinator.hpp`
- `src/distributed/Coordinator.cpp`
- `include/distributed/IngestorNode.hpp`
- `src/distributed/IngestorNode.cpp`
- `docker-compose.cluster.yml` — multi-node deployment

**Files to modify:** `src/serving/main_server.cpp` (CLI mode parsing),
`CMakeLists.txt` (gRPC codegen, new library target), `Dockerfile` (install gRPC)

**Tests:**
- `tests/unit/test_coordinator.cpp`:
  - Worker registration and heartbeat tracking
  - Game assignment to least-loaded worker
  - Failure detection after 3 missed heartbeats → reassignment
  - Epoch fencing: stale epoch rejected
  - New game discovered → assigned; finished game → revoked
- `tests/unit/test_ingestor_node.cpp`:
  - ASSIGN starts polling, REVOKE stops polling
  - Epoch validation rejects stale assignments
  - Stats reported to coordinator on heartbeat
- `tests/integration/test_cluster.cpp` (via docker-compose):
  - Start coordinator + 3 workers
  - Simulate 5 live games → verify all assigned across workers
  - Kill worker-2 → verify its games reassigned within 10 seconds
  - Restart worker-2 → verify it re-registers and gets new assignments
  - All stats queryable via coordinator HTTP API throughout

**Acceptance criteria:**
- 3-worker cluster distributes live games across nodes
- Worker failure detected within 6 seconds (3 missed heartbeats)
- Failed worker's games reassigned to survivors automatically
- Epoch fencing prevents split-brain (revived worker can't operate on old assignments)
- `--mode=standalone` behavior is 100% unchanged (no regressions)
- State recovery via replay produces identical stats to continuous ingestion
- `docker compose -f docker-compose.cluster.yml up` starts full cluster
- Coordinator HTTP API serves aggregated stats from all workers

---

### 8.3 — Consistent Hashing & Health-Aware Routing

**Problem:** Phase 8.2's coordinator uses naive least-loaded assignment. If you add a 4th
worker, existing game assignments don't rebalance efficiently. Game-to-worker mapping is
ad-hoc, not deterministic. This is the difference between "I built a cluster" and "I
understand distributed systems."

**Work:**
1. **Create `include/distributed/ConsistentHashRing.hpp`:**
   - Hash ring using jump consistent hashing or virtual nodes (150 vnodes per physical node)
   - Hash function: xxHash on game_id → position on ring
   - `assign(game_id)` → returns the worker that owns the ring segment
   - `add_node(worker_id)` → adds vnodes, returns set of game_ids that need migration
     (only ~1/N of total games move)
   - `remove_node(worker_id)` → removes vnodes, returns game_ids that need reassignment
     to successor nodes

2. **Create `src/distributed/ConsistentHashRing.cpp`** — Implementation.

3. **Integrate into Coordinator:**
   - Replace least-loaded assignment with hash ring lookup
   - On worker join: add to ring, compute migration set, send REVOKE to old owners +
     ASSIGN to new owner for migrated games only
   - On worker failure: remove from ring, reassign only the failed worker's games to their
     new ring successors
   - Log migration: "Worker {id} added — migrating {N}/{total} games ({pct}%)"

4. **Health-aware routing:**
   - Coordinator tracks per-worker health score: heartbeat freshness + event throughput +
     reported CPU load
   - If a worker is "degraded" (heartbeat late but not dead, CPU > 90%), coordinator
     avoids assigning new games to it but doesn't revoke existing ones
   - Three health states: `Healthy`, `Degraded`, `Dead`
   - Health state exposed in coordinator's `/health` endpoint:
     ```json
     {
       "cluster": {
         "workers": [
           {"id": "w-1", "status": "healthy", "games": 12, "events_sec": 450},
           {"id": "w-2", "status": "degraded", "games": 8, "events_sec": 120},
           {"id": "w-3", "status": "dead", "games": 0, "last_seen": "2s ago"}
         ],
         "total_games": 20,
         "unassigned_games": 0
       }
     }
     ```

5. **Graceful worker shutdown:**
   - Worker sends a `Deregister` RPC before exiting (on SIGTERM)
   - Coordinator treats this as a planned removal — no failure detection delay
   - Games migrated immediately to ring successors

**Files to create:** `include/distributed/ConsistentHashRing.hpp`,
`src/distributed/ConsistentHashRing.cpp`

**Files to modify:** `src/distributed/Coordinator.cpp`, `proto/cortex.proto`
(add Deregister RPC), `include/distributed/IngestorNode.hpp` (graceful shutdown)

**Tests:**
- `tests/unit/test_hash_ring.cpp`:
  - Distribution uniformity: 100 games across 3 nodes → each gets 28-38 (within 15%)
  - Node addition moves only ~1/N keys
  - Node removal reassigns only the removed node's keys
  - Deterministic: same game_id always maps to same node (given same ring)
  - Empty ring returns error
- `tests/unit/test_health_routing.cpp`:
  - Healthy worker receives new assignments
  - Degraded worker keeps existing games, gets no new ones
  - Dead worker's games migrate to ring successors
  - Graceful shutdown triggers immediate migration (no 6s delay)

**Acceptance criteria:**
- Adding a 4th worker migrates only ~25% of games (not 100% reshuffle)
- Removing a worker reassigns only its games to ring successors
- Health states (healthy/degraded/dead) visible in `/health` response
- Graceful shutdown (SIGTERM) migrates games with zero detection delay
- Distribution is within 15% of perfectly uniform across 3+ nodes
- All hash ring operations are O(log N) in number of vnodes

---

### 8.4 — AVX2/SSE4 SIMD Path for x86

**Problem:** SIMD code only has ARM NEON path. x86 CI builds use scalar fallback.
Demonstrates platform expertise if both paths exist.

**Work:**
1. Add `#ifdef __x86_64__` paths in `GameStateIndex.cpp` and `HNSWIndex.cpp`:
   - SSE4.1: `_mm_load_ps`, `_mm_sub_ps`, `_mm_mul_ps`, `_mm_hadd_ps`
   - AVX2 (if available): `_mm256_load_ps` for 8-wide processing
2. Runtime SIMD dispatch via `__builtin_cpu_supports("avx2")` or compile-time `#ifdef`.
3. Ensure CI benchmarks run on x86 (Ubuntu runner) and report SIMD path used.

**Files to modify:** `src/analytics/GameStateIndex.cpp`, `src/analytics/HNSWIndex.cpp`

**Tests:** Existing similarity tests must pass on both architectures.
CI benchmark reports which SIMD path was used.

**Acceptance criteria:**
- x86 CI build uses SSE4/AVX2, not scalar fallback
- ARM CI build uses NEON
- Benchmark output logs SIMD backend name
- Results are bit-identical (within float tolerance) across all paths

---

## Phase 9 — Testing & Reliability

**Goal:** Move test coverage from "good enough" to "would satisfy a Jane Street or Stripe
reliability bar." Focus on property-based testing, fuzz testing, and failure injection.

**Target scores:** Code Quality 8.5→9.5, Production Readiness 8.5→9

### 9.1 — Property-Based Testing

**Problem:** All tests use hand-written examples. No generative testing to find edge cases
the developer didn't think of.

**Work:**
1. Add [RapidCheck](https://github.com/emil-e/rapidcheck) as a test dependency (CMake
   FetchContent).
2. Write property tests for:
   - **RingBuffer:** For any sequence of N pushes and M pops (M ≤ N), total popped equals
     total pushed in FIFO order. Concurrent SPSC: producer pushes K items, consumer pops K
     items — no duplicates, no losses, correct order.
   - **StatAccumulator:** Accumulating any sequence of events produces non-negative stat
     counts. Snapshot is always consistent (points = 2pt*2 + 3pt*3 + ft*1).
   - **Router:** For any valid path pattern and matching URL, parameters are extracted
     correctly. Non-matching URLs return 404.
   - **EloTracker:** After processing any permutation of games, final Elo sum is conserved
     (within float tolerance).
   - **RateLimiter:** After N requests in T seconds, at most (burst + T * rate) are allowed.
3. Target: 10+ property tests across 5+ components.

**Files to create:** `tests/property/test_properties.cpp`

**Files to modify:** `CMakeLists.txt` (add property test executable)

**Acceptance criteria:**
- 10+ property tests pass
- RapidCheck runs 1000+ cases per property
- At least one property test covers concurrent behavior
- CI runs property tests on every push

---

### 9.2 — Fuzz Testing

**Problem:** HTTP parser and WebSocket frame parser accept untrusted input but are not fuzz
tested. These are the attack surface.

**Work:**
1. Create fuzz harnesses using libFuzzer (Clang built-in):
   - `tests/fuzz/fuzz_http_parser.cpp` — Feeds random bytes to llhttp + request processing
   - `tests/fuzz/fuzz_ws_frame.cpp` — Feeds random bytes to WebSocket frame parser
   - `tests/fuzz/fuzz_json_response.cpp` — Feeds random strings as player names/game IDs
     to JSON serializer, verifies output is valid JSON
2. Add CMake targets: `cortex_fuzz_http`, `cortex_fuzz_ws`, `cortex_fuzz_json`.
3. Seed corpus from captured real HTTP requests and WebSocket frames.
4. CI: Run each fuzzer for 60 seconds on every push (find regressions fast).

**Files to create:** `tests/fuzz/fuzz_http_parser.cpp`, `tests/fuzz/fuzz_ws_frame.cpp`,
`tests/fuzz/fuzz_json_response.cpp`

**Files to modify:** `CMakeLists.txt`, `.github/workflows/ci.yml`

**Acceptance criteria:**
- All 3 fuzzers run without crashes for 60+ seconds
- Any crash is a test failure in CI
- Corpus checked into `tests/fuzz/corpus/`
- Found bugs (if any) are fixed before merging

---

### 9.3 — CI Performance Regression Tests

**Problem:** Benchmarks exist but are not part of CI. Performance regressions can silently
ship.

**Work:**
1. Add a CI job that runs `bench_throughput` and `bench_similarity` on every push.
2. Parse benchmark output and compare against baseline thresholds:
   - Ring buffer: ≥5M events/sec (conservative threshold)
   - Similarity search: ≤10ms p99 at 100K vectors
3. Store baseline as a JSON file in `tests/benchmarks/baselines.json`.
4. CI job fails if any metric regresses >20% from baseline.
5. Add benchmark results as a GitHub Actions job summary annotation.

**Files to create:** `tests/benchmarks/baselines.json`,
`scripts/check_benchmark_regression.py`

**Files to modify:** `.github/workflows/ci.yml`

**Acceptance criteria:**
- Benchmark CI job runs on every push
- >20% regression fails the build
- Job summary shows benchmark results in a table
- Baselines are checked into version control

---

## Phase 10 — Observability & Operational Excellence

**Goal:** Demonstrate the operational maturity that separates a project from an application.
Show that you think about running software, not just writing it.

**Target scores:** Production Readiness 9→9.5, Product Completeness 8→9

### 10.1 — Health Check Depth & Readiness Probes

**Problem:** `/health` returns a static 200 OK. It doesn't check whether the system is
actually healthy (DB reachable, Redis reachable, ring buffer not full).

**Work:**
1. Expand `/health` to a deep health check:
   ```json
   {
     "status": "healthy",
     "checks": {
       "database": {"status": "up", "latency_ms": 1.2},
       "redis": {"status": "up", "latency_ms": 0.3},
       "ring_buffer": {"status": "ok", "utilization_pct": 12.5},
       "elo_tracker": {"status": "built", "games_processed": 8434}
     },
     "uptime_seconds": 3621
   }
   ```
2. Add `/ready` endpoint — returns 200 only when all dependencies are connected and
   Elo ratings are built. Used by Kubernetes/Docker health checks.
3. Return 503 from `/health` if any critical dependency is down.

**Files to modify:** Health handler, `Dockerfile` (update HEALTHCHECK)

**Acceptance criteria:**
- `/health` checks DB, Redis, ring buffer, Elo status
- `/ready` returns 503 until all deps are connected
- Docker HEALTHCHECK uses `/ready`
- Degraded component shows status "degraded" not crash

---

### 10.2 — Graceful Degradation & Circuit Breaking

**Problem:** Redis failures are handled with try/catch that logs and continues. No circuit
breaker pattern — every request retries a dead Redis, adding latency.

**Work:**
1. Create `include/serving/CircuitBreaker.hpp` — States: closed (normal), open (fast-fail),
   half-open (probe). Configurable failure threshold (5), reset timeout (30s).
2. Wrap Redis calls in circuit breaker. When open, skip cache entirely (serve from DB).
3. Wrap DB calls in circuit breaker. When open, return 503 immediately.
4. Expose circuit breaker state in `/health` response and Prometheus metrics.

**Files to create:** `include/serving/CircuitBreaker.hpp`

**Files to modify:** `RedisCache.cpp`, handler files, metrics handler

**Tests:** `tests/unit/test_circuit_breaker.cpp` — state transitions, threshold counting,
timeout-based reset, half-open probe.

**Acceptance criteria:**
- Circuit breaker transitions: closed → open after 5 failures, open → half-open after 30s
- Redis circuit open: requests served from DB without cache latency penalty
- DB circuit open: immediate 503 (no hanging connections)
- Circuit state visible in `/health` and `/metrics`

---

### 10.3 — Backpressure Throughout Pipeline

**Problem:** Backpressure only exists at the WebSocket output queue (1024 frame cap). No
backpressure from ring buffer to HTTP server, or from DB pool exhaustion to request handling.

**Work:**
1. Ring buffer full → LiveIngestor already handles (log + drop). Add a Prometheus counter
   for dropped events: `cortex_events_dropped_total`.
2. DB pool exhausted → Return 503 with `Retry-After` header instead of blocking indefinitely.
3. Add overall request queue depth metric. If pending requests exceed threshold (e.g., 500),
   new requests get 503 (load shedding).
4. Expose all backpressure metrics in Prometheus.

**Files to modify:** `ConnectionPool.hpp`, handler files, metrics handler, `LiveIngestor.cpp`

**Tests:** Backpressure test — simulate pool exhaustion, verify 503 + Retry-After.

**Acceptance criteria:**
- Dropped events counted in Prometheus
- DB pool exhaustion returns 503, not hang
- Load shedding kicks in at configurable request depth
- All backpressure metrics visible in Grafana dashboard

---

## Phase 11 — Frontend & Developer Experience

**Goal:** Polish the dashboard and developer workflow to demonstrate product thinking,
not just backend engineering.

**Target scores:** Product Completeness 8→9, Resume Strength +0.5

### 11.1 — API Documentation (OpenAPI)

**Work:**
1. Create `docs/openapi.yaml` — Full OpenAPI 3.0 spec for all REST endpoints.
   Include request/response schemas, path parameters, query parameters, error responses,
   authentication requirements.
2. Serve Swagger UI at `/docs` (embed swagger-ui dist or use CDN).
3. Validate API responses against OpenAPI schema in integration tests.

**Files to create:** `docs/openapi.yaml`

**Files to modify:** Static handler (serve `/docs`), integration tests

**Acceptance criteria:**
- All 12+ endpoints documented in OpenAPI spec
- Swagger UI accessible at `/docs`
- At least 5 endpoints validated against schema in tests

---

### 11.2 — Dashboard Live Refresh & UX Polish

**Work:**
1. Add auto-refresh toggle for leaderboard data (30-second interval).
2. Add loading skeletons while data fetches (replace blank states).
3. Add error toasts for failed API calls (not silent failures).
4. Add dark/light mode toggle (CSS custom properties).
5. Add responsive breakpoints for mobile viewing.

**Files to modify:** `www/index.html`

**Acceptance criteria:**
- Auto-refresh works with visual indicator
- No blank/broken states during loading
- Dashboard usable on mobile viewport (375px width)
- Dark mode toggle persists via localStorage

---

## Agent Execution Guide

Each phase is designed to be executed by a Claude Code agent session. Recommended execution
order and parallelism:

```
Phase 6.1 (Router)              ──── MUST BE FIRST ────
    │
    ├── Phase 6.2 (JSON)             ← depends on 6.1 (handler files exist)
    ├── Phase 6.3 (DI)               ← depends on 6.1 (ServerContext exists)
    │
Phase 7.1 (Connection Pool)     ← depends on 6.1 (handlers use pool)
Phase 7.2 (Tracing)             ← depends on 6.1 (Request/Response objects exist)
Phase 7.3 (Pagination)          ← depends on 6.1 + 6.2 (handlers + JSON)
Phase 7.4 (Auth)                ← depends on 6.1 (router middleware)
    │
Phase 8.1 (HNSW)                ← independent (analytics layer)
Phase 8.2 (Distributed cluster) ← depends on 6.1 (CLI mode parsing) — BIG, schedule early
Phase 8.3 (Consistent hashing)  ← depends on 8.2 (coordinator exists)
Phase 8.4 (AVX2)                ← depends on 8.1 (same distance functions)
    │
Phase 9.1 (Property tests)      ← can run after 6.1
Phase 9.2 (Fuzz tests)          ← can run after 6.1
Phase 9.3 (CI benchmarks)       ← independent
    │
Phase 10.1 (Health)             ← depends on 7.1 (pool health)
Phase 10.2 (Circuit breaker)    ← depends on 7.1
Phase 10.3 (Backpressure)       ← depends on 7.1 + 10.2
    │
Phase 11.1 (OpenAPI)            ← depends on 7.3 + 7.4 (pagination + auth in spec)
Phase 11.2 (Dashboard)          ← independent
```

**Agent instructions per phase:**
1. Read this ROADMAP.md and the referenced source files before starting.
2. Implement exactly what is specified — no extra features, no skipped requirements.
3. Run `cmake --build build` after changes to verify compilation.
4. Run `ctest --test-dir build --output-on-failure` to verify no regressions.
5. Check acceptance criteria — every criterion must be met before marking complete.
6. Update the status table at the top of this file when the phase is complete.

**Parallelizable agent sessions (after Phase 6.1 is done):**
- Agent A: Phase 6.2 + 6.3 (JSON + DI)
- Agent B: Phase 7.1 + 7.2 (Pool + Tracing)
- Agent C: Phase 8.1 (HNSW — long, independent)
- Agent D: Phase 8.2 + 8.3 (Distributed cluster — largest work item, start early)
- Agent E: Phase 9.1 + 9.2 (Property + Fuzz tests)
- Agent F: Phase 11.2 (Dashboard — independent)

---

## Projected Scores After Completion

| Criterion | Current | Target | Key Phases |
|-----------|---------|--------|------------|
| Technical Difficulty | 7.5 | 9.5 | 8.1 (HNSW), 8.2 (distributed stream cluster), 8.4 (multi-arch SIMD) |
| Engineering Complexity | 7.0 | 9.5 | 8.2 (coordinator + workers), 8.3 (consistent hashing), 7.1 (pool), 10.2 (circuit breaker) |
| Code Quality | 7.5 | 9.0 | 6.1 (router), 6.2 (JSON), 6.3 (DI) |
| Architecture | 6.5 | 9.5 | 6.1 (decomposition), 8.2 (distributed), 8.3 (hash ring), 7.1 (async) |
| Product Completeness | 7.0 | 9.0 | 7.3 (pagination), 7.4 (auth), 11.1 (OpenAPI) |
| Production Readiness | 5.0 | 9.0 | 7.1 (pool), 10.1 (health), 10.2 (circuit breaker), 10.3 (backpressure) |
| Resume Strength | 7.0 | 9.5 | 8.2 is the headline feature — "distributed stream processing with failure recovery" |
| Differentiation | 7.0 | 9.5 | 8.2 (distributed cluster), 8.3 (consistent hashing), 8.1 (HNSW), 9.1 (property tests) |
