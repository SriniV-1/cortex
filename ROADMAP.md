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
| Shell + Python scripts | ~615 | 3 |
| CMake build | 195 | 1 |
| **Total** | **~9,200** | **38** |

---

## Key File Map

```
CORTEX/
├── ROADMAP.md              <- this file (keep updated)
├── README.md               <- user-facing docs
├── PROJECT_DESCRIPTION.md  <- resume/LinkedIn handoff
├── CMakeLists.txt          <- root build (7 executables + 4 static libraries)
├── cortex.sh               <- start/stop/load/bench convenience script
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
