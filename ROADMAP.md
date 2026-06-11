# CORTEX — Real-Time NBA Analytics Engine
## C++20 · PostgreSQL 15 · Redis 7 · ONNXRuntime · ARM NEON SIMD

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
| **Phase 1** | Playoff ETL with gap tolerance | Done | 120-miss threshold for inter-round ID gaps (covers Finals at ID 401+) |
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
| **Phase 4** | Automatic data ingestion | Done | Startup refresh + 30s game completion callback + daily 4 AM full scan; Elo rebuild on new data |
| **Phase 5** | GitHub Actions CI | Done | Matrix build (Ubuntu + macOS), ctest, ASan/UBSan on every push |
| **Phase 5** | Docker containerization | Done | Multi-stage Dockerfile, docker-compose (Postgres + Redis + Cortex) |
| **Phase 5** | CMake portability | Done | Replace hardcoded /opt/homebrew paths with find_package/pkg_check_modules |
| **Phase 5** | Integration tests | Done | 6 end-to-end tests: schema, games, events, Elo determinism, API health, leaderboard |
| **Phase 5** | API rate limiting | Done | Token bucket rate limiter (50 req/sec, 100 burst), 429 on exceeded, per-IP tracking |
| **Phase 5** | Enriched Prometheus metrics | Done | 5 metric families: events, active games, rate limiter, similarity index, Elo |
| **Phase 5** | Grafana observability | Done | Pre-built dashboard JSON (8 panels) + Prometheus scrape config |
| **Phase 5** | Frontend data visualization | Done | Chart.js Elo trajectory (line) + rating distribution (bar), /api/elo/history endpoint |
| **Phase 6** | Router abstraction & HttpServer decomposition | Done | Trie-based router, Request/Response types, handler files, ServerContext DI |
| **Phase 6** | Structured JSON serialization | Done | nlohmann/json replaces ostringstream builders |
| **Phase 6** | Dependency injection & testability | Done | ICache/IDatabase interfaces, mock-based handler tests |
| **Phase 7** | Connection pooling & async DB | Done | Fixed-size pqxx pool, DB queries off event loop thread |
| **Phase 7** | Request tracing & structured logging | Done | UUID trace_id per request, X-Trace-Id header, structured log lines |
| **Phase 7** | Cursor-based API pagination | Done | Base64 cursors, default limit=50, has_more flag |
| **Phase 7** | JWT authentication & RBAC | Done | Bearer token validation, viewer/admin roles, auth middleware |
| **Phase 8** | HNSW approximate nearest neighbor index | Done | Hand-rolled HNSW graph, >95% recall@10, <1ms query at 4.7M vectors |
| **Phase 8** | Distributed stream processing cluster | Done | gRPC coordinator-worker architecture, epoch fencing, failure detection, state handoff |
| **Phase 8** | Consistent hashing & shard health routing | Done | FNV-1a hash ring with 150 virtual nodes, health-aware routing, graceful deregister |
| **Phase 8** | AVX2/SSE4 SIMD path for x86 | Done | Dual SIMD backend (NEON + AVX2), runtime dispatch |
| **Phase 9** | Property-based testing | Done | RapidCheck: 10+ properties across 5 components |
| **Phase 9** | Fuzz testing | Done | libFuzzer harnesses for HTTP parser, WS frames |
| **Phase 9** | CI performance regression tests | Done | Benchmark baselines, >20% regression fails build |
| **Phase 10** | Deep health checks & readiness probes | Done | /health checks all deps, /ready for orchestrator probes |
| **Phase 10** | Circuit breaking & graceful degradation | Done | 3-state circuit breaker on Redis + DB |
| **Phase 10** | Backpressure throughout pipeline | Done | Load shedding, 503+Retry-After, dropped event counters |
| **Phase 11** | OpenAPI documentation | Done | Full OpenAPI 3.0 spec, Swagger UI at /docs |
| **Phase 11** | Dashboard UX polish | Done | Auto-refresh, loading states, dark mode, mobile responsive |

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
- **Playoff ID gaps:** 120-miss early termination threshold (vs 30 for regular season) because playoff game IDs have natural gaps between rounds (~85 IDs between conference finals and Finals)

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

**Result:**
- GitHub Actions CI: matrix build (Ubuntu + macOS), ctest, ASan/UBSan on every push
- Multi-stage Dockerfile + docker-compose (Postgres + Redis + Cortex)
- CMake portability via find_package/pkg_check_modules (no hardcoded paths)
- 6 integration tests: schema, games, events, Elo determinism, API health, leaderboard
- Token bucket rate limiter (50 req/sec, 100 burst), 429 on exceeded, per-IP tracking
- 5 Prometheus metric families: events, active games, rate limiter, similarity index, Elo

- Grafana dashboard (8 panels) + Prometheus scrape config
- Chart.js Elo trajectory + rating distribution charts with `/api/elo/history` endpoint

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
| C++ source + headers | ~9,900 | 74 |
| Unit + property + fuzz tests | ~3,700 | 19 |
| Frontend (HTML/CSS/JS) | ~1,100 | 1 |
| SQL schema | 228 | 1 |
| Protobuf (gRPC service def) | 80 | 1 |
| Shell + Python scripts | ~615 | 3 |
| CMake build | 358 | 1 |
| CI/CD + Docker + Compose | ~200 | 5 |
| **Total** | **~16,200** | **105** |

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
├── docker-compose.cluster.yml <- distributed 3-worker cluster (Phase 8)
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
│   ├── analytics/          <- WinProbModel, GameStateIndex, EloTracker, HNSWIndex
│   └── distributed/        <- Coordinator, IngestorNode, ConsistentHashRing
├── src/
│   ├── etl/                <- NBAClient.cpp, BulkInserter.cpp, LiveIngestor.cpp, main_etl.cpp
│   ├── stream/             <- StreamProcessor.cpp, StatAccumulator.cpp
│   ├── serving/            <- KqueuePoller.cpp, HttpServer.cpp, Router.cpp, RedisCache.cpp, handlers/, main_server.cpp
│   ├── analytics/          <- WinProbModel.cpp, GameStateIndex.cpp, EloTracker.cpp, HNSWIndex.cpp
│   └── distributed/        <- Coordinator.cpp, IngestorNode.cpp, ConsistentHashRing.cpp
├── scripts/
│   ├── bootstrap_db.sh     <- creates DB, runs schema.sql
│   └── train_win_prob.py   <- model training (scikit-learn -> ONNX export)
├── data/
│   └── models/
│       └── win_prob.onnx   <- 7-feature logistic regression (217 bytes)
├── proto/
│   └── cortex.proto        <- gRPC service definition (Phase 8)
├── tests/
│   ├── unit/               <- 43 gtest tests across 12 suites
│   ├── property/           <- RapidCheck property-based tests (Phase 9)
│   ├── fuzz/               <- libFuzzer harnesses (Phase 9)
│   ├── integration/        <- end-to-end tests (Phase 5)
│   └── benchmarks/         <- 5 benchmark executables
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
| 4 | Playoff ETL missing conference finals / finals | Increased early-termination threshold to 120 for playoffs; expanded ID range to 500 (Finals start at ID 401) |
| 5 | WebSocket never auto-connected to live games | `connectStream()` was defined but never called; added auto-subscribe on live game detection |
| 6 | Segfault on Ctrl+C shutdown | Background jthreads used non-interruptible `sleep_for()`; replaced with 1-sec tick loop checking `stop_requested()` |
| 7 | StatAccumulator maps grew monotonically | Added time-based eviction (4-hour staleness, 10-minute sweep interval) |
| 8 | WebSocket outbound queue unbounded | Capped at 1024 frames per connection; slow clients disconnected |
| 9 | `shared_mutex` deadlock in EloTracker | `build_from_db()` held unique_lock then called method needing shared_lock; inlined the logic |
| 10 | Live games never appeared in dashboard | `/api/games/recent` only queried DB (status=3). Added `/api/scoreboard` backed by LiveIngestor's cached scoreboard snapshot; frontend now fetches both endpoints in parallel |
| 11 | Dashboard "connecting..." — JS never executed | Temporal Dead Zone: `applyTheme()` accessed `eloHistoryData` (declared with `let` ~800 lines later) during IIFE at page load. Wrapped in try/catch |
| 12 | Games/Elo rankings not rendering | API returns `{data: [...]}` envelope but JS called `.forEach()` directly on response. Fixed to unwrap `.data` property |
| 13 | Elo trajectory showing bad teams as top-ranked | `loadElo()` and `loadEloHistory()` ran in parallel; chart defaulted to alphabetical teams. Fixed by awaiting `loadElo()` first |
| 14 | Daily refresh only inserted play events, not game metadata | `main_server.cpp` refresh called only `fetch_play_by_play` + `bulk_insert_play_by_play`, skipping boxscore/teams/players. Added full ETL pipeline (boxscore → teams → players → game → play-by-play) |
| 15 | Completed games delayed until 4 AM daily refresh | Added `on_game_complete` callback to LiveIngestor: when a tracked live game transitions to status=3, full persist + Elo rebuild fires within the same 30s poll cycle |
| 16 | No startup data catch-up | Daily refresher waited until 4 AM before first run. Changed to run immediately on startup, then schedule 4 AM for subsequent runs |
| 17 | protoc fails on paths with spaces | Project path "CS Projects" caused `--proto_path` to fail. Fixed by copying .proto into build dir and using `WORKING_DIRECTORY` with relative paths |


---

## Phase 6 — Architectural Hardening

**Result:**
- Trie-based `Router` with path parameter extraction (`:param` syntax), replacing 800-line if-chain
- `Request`/`Response` typed wrappers, `ServerContext` dependency container
- 10 handler files under `src/serving/handlers/` — `HttpServer.cpp` reduced to ~200 lines
- nlohmann/json for all outbound JSON serialization (simdjson still used for parsing)
- `ICache`/`IDatabase` interfaces for mock-based handler unit tests

---

## Phase 7 — Production-Grade Infrastructure

**Result:**
- Fixed-size `pqxx::connection` pool with health-checked checkout/return
- DB queries executed off the kqueue event loop via thread pool
- UUID v4 `trace_id` per request, `X-Trace-Id` response header, structured log lines
- Cursor-based pagination (base64 cursors, `has_more` flag, default limit=50)
- JWT authentication with `viewer`/`admin` roles, `POST /api/auth/token` endpoint

---

## Phase 8 — Distributed Stream Processing & Advanced Analytics

**Result:**
- Hand-rolled HNSW graph index: >95% recall@10, <1ms query at 4.7M vectors
- gRPC coordinator-worker cluster (modeled after Kafka consumer groups):
  - Coordinator: game assignment, 3-state health tracking (Healthy/Degraded/Dead), epoch fencing
  - IngestorNode workers: per-game poll threads, SPSC ring buffer, 2s heartbeat
  - Consistent hash ring: FNV-1a with 150 virtual nodes, minimal key migration on join/leave
  - Graceful deregister on SIGTERM for zero-delay game migration
- `docker-compose.cluster.yml`: 1 coordinator + 3 workers + Postgres + Redis
- `--mode standalone|coordinator|worker` CLI flags
- Dual SIMD backend: ARM NEON + AVX2/SSE4, compile-time platform selection

---

## Phase 9 — Testing & Reliability

**Result:**
- RapidCheck property-based tests: 10+ properties across RingBuffer, StatAccumulator, Router, EloTracker, RateLimiter
- libFuzzer harnesses for HTTP parser and WebSocket frame parser
- CI benchmark regression tests with baseline thresholds

---

## Phase 10 — Observability & Operational Excellence

**Result:**
- Deep `/health` checks (DB, Redis, ring buffer, Elo status) + `/ready` probe
- 3-state circuit breaker (closed/open/half-open) on Redis and DB
- Load shedding at configurable request depth, 503+Retry-After on pool exhaustion
- Dropped event Prometheus counters

---

## Phase 11 — Frontend & Developer Experience

**Result:**
- Full OpenAPI 3.0 spec, Swagger UI at `/docs`
- Dashboard: auto-refresh toggle, loading skeletons, error toasts, dark/light mode, mobile responsive
