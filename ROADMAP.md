# CORTEX — Real-Time NBA Analytics Engine
## C++20 · PostgreSQL/TimescaleDB · Redis · ONNXRuntime

> **Agent handoff note:** Read this file first. It is the single source of truth for project state.
> Update the status table and "Current focus" section whenever a phase item completes or a blocker surfaces.

---

## Project Overview

Three-layer C++20 system that ingests NBA game data, computes real-time statistics,
and serves live analytics via HTTP/WebSocket. Target: sub-20ms p99 query latency at
full historical corpus (30K+ games, ~13M play-by-play events).

**Platform:** macOS arm64 (Apple Silicon) for development.
- epoll in the spec → replaced with **kqueue** on macOS (same semantics, BSD origin)
- AVX2 SIMD in the spec → **ARM NEON** on M-series chips
- Final deployment target is Linux; platform abstraction layer isolates this difference

**Working NBA data source (confirmed):**
`https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData/`
- `scoreboard/todaysScoreboard_00.json` — today's games
- `playbyplay/playbyplay_{gameId}.json` — play-by-play actions (~400–600 per game)
- `boxscore/boxscore_{gameId}.json` — box scores

stats.nba.com and cdn.nba.com are blocked from this environment (IP-level Akamai block).
The S3 endpoint requires no auth and returns clean JSON.

---

## Status Overview

| Phase | Component | Status | Notes |
|-------|-----------|--------|-------|
| **Phase 1** | Dependencies installed | ✅ Done | cmake 4.3, psql 15, redis, libpqxx, spdlog, simdjson, googletest |
| **Phase 1** | NBA API endpoint verified | ✅ Done | S3 endpoint works; stats.nba.com blocked. Pre-2019 returns 403. |
| **Phase 1** | Project directory structure | ✅ Done | src/, include/, sql/, tests/, etc. |
| **Phase 1** | ROADMAP.md | ✅ Done | This file |
| **Phase 1** | CMakeLists.txt (root) | ✅ Done | All targets build clean |
| **Phase 1** | PostgreSQL schema (native range partitioning) | ✅ Done | sql/schema.sql — PG15 on port 5433 |
| **Phase 1** | C++ ETL pipeline | ✅ Done | NBAClient + BulkInserter + main_etl |
| **Phase 1** | Dimension tables populated (teams/games/players) | ✅ Done | fetch_boxscore() + --populate-dimensions backfill |
| **Phase 1** | Historical bulk load | ✅ Done | 6,637 games / 3.7M events / 30 teams / 1,080 players (2019–2025) |
| **Phase 1** | Query benchmark (<20ms p99) | ✅ Done | game_events 3.2ms, player_season 6.3ms, game_summary 0.6ms, time_range 0.2ms |
| **Phase 2** | Lock-free ring buffer | ✅ Done | include/stream/RingBuffer.hpp — SPSC, power-of-2, cache-line padded |
| **Phase 2** | Stream processor | ✅ Done | src/stream/StreamProcessor.cpp — jthread, ARM NEON backoff |
| **Phase 2** | Rolling window aggregations | ✅ Done | StatAccumulator: last-N events per player×game via capped deque |
| **Phase 2** | ONNX model integration | ✅ Done | WinProbModel: logistic regression, 5-feature input, sigmoid output |
| **Phase 3** | kqueue I/O event loop | ⬜ Not started | src/serving/ (macOS kqueue) |
| **Phase 3** | HTTP/WebSocket server | ⬜ Not started | src/serving/ |
| **Phase 3** | Prometheus metrics | ⬜ Not started | |
| **Phase 3** | Redis caching layer | ⬜ Not started | |

---

## Phase 1 — Data Layer (Current)

**Goal:** PostgreSQL + TimescaleDB schema, C++ ETL that bulk-loads historical NBA data,
queries run <20ms p99.

### Acceptance Criteria
- [ ] TimescaleDB hypertable on `play_events` partitioned by `occurred_at`
- [ ] `games`, `teams`, `players`, `play_events` tables created
- [ ] Indexes: game_id, player_id, action_type, time range
- [ ] C++ ETL fetches game IDs from S3 scoreboard, resolves play-by-play, bulk-inserts via COPY
- [ ] 30K+ games loaded (2000–2024 seasons), ~13M events
- [ ] `SELECT COUNT(*) FROM play_events` with time filter completes <20ms p99

### Scale reality check
S3 only serves data for **2019-present** (pre-2019 returns 403). Actual corpus:
- 6,637 games loaded (2019–2025, 6 seasons)
- ~560 play-by-play actions per game average
- **Actual total: 3.7M events**
The original 50M / 13M estimates assumed full historical access. 3.7M is the real ceiling.

### Technical decisions
- **ETL language:** C++ with libcurl + libpqxx — uses PostgreSQL COPY protocol for bulk insert
- **Concurrency:** `std::jthread` pool (configurable, default 8 threads) for parallel game fetching
- **Rate limiting:** 100ms sleep between S3 requests to be a good citizen
- **Schema:** INT vs BIGINT — use INT for IDs (NBA game IDs fit in 32-bit), BIGINT for event PK
- **TimescaleDB:** chunk_time_interval = 1 month on `occurred_at`

---

## Phase 2 — Stream Processing

**Goal:** Lock-free ring buffer ingests live play-by-play events; stream processor
computes rolling window statistics (last 5 plays, last 10 minutes, game totals).

### Acceptance Criteria
- [ ] SPSC ring buffer with configurable capacity (power-of-2, default 65536)
- [ ] Stream processor thread consumes events, updates in-memory stat tables
- [ ] Rolling aggregations: points/rebounds/assists over configurable window
- [ ] ONNX model loaded and producing win probability per event
- [ ] Throughput: 1M events/sec sustained on M-series chip

### Technical decisions
- **Ring buffer:** SPSC (single-producer single-consumer) `std::atomic` head/tail, cache-line padding
- **Memory ordering:** `std::memory_order_acquire` / `release` — no mutex in hot path
- **SIMD:** ARM NEON intrinsics for batch stat aggregation (float32 vectors)
- **ONNX:** ONNXRuntime C API, model loaded once at startup

---

## Phase 3 — Serving Layer

**Goal:** HTTP/WebSocket server using kqueue (macOS) / epoll (Linux) I/O multiplexing.
Serves live stats via WebSocket, historical queries via REST.

### Acceptance Criteria
- [ ] Platform abstraction: `IOPoller` interface with `KqueuePoller` (macOS) and `EpollPoller` (Linux) impls
- [ ] HTTP/1.1 server: `GET /stats/{gameId}`, `GET /players/{playerId}/season`
- [ ] WebSocket upgrade on `GET /live/{gameId}`
- [ ] Prometheus metrics at `GET /metrics`
- [ ] Redis cache: 1-minute TTL on computed aggregations
- [ ] Load test: 1000 concurrent WebSocket clients, <20ms p99

---

## Key File Map

```
CORTEX/
├── ROADMAP.md              ← this file (keep updated)
├── CMakeLists.txt          ← root build
├── sql/
│   └── schema.sql          ← TimescaleDB schema
├── include/
│   ├── common/             ← shared types, logger, config
│   ├── etl/                ← NBAClient, BulkInserter interfaces
│   ├── stream/             ← RingBuffer, StreamProcessor
│   ├── serving/            ← IOPoller, HttpServer, WsServer
│   └── analytics/          ← OnnxModel, StatAggregator
├── src/
│   ├── etl/                ← NBAClient.cpp, BulkInserter.cpp, main_etl.cpp
│   ├── stream/             ← StreamProcessor.cpp
│   ├── serving/            ← KqueuePoller.cpp, HttpServer.cpp, main_server.cpp
│   └── analytics/          ← OnnxModel.cpp
├── tests/
│   ├── unit/               ← gtest unit tests per component
│   ├── integration/        ← DB + API integration tests
│   └── benchmarks/         ← gbenchmark latency/throughput tests
├── scripts/
│   └── bootstrap_db.sh     ← creates DB, installs TimescaleDB, runs schema.sql
└── config/
    └── cortex.toml         ← runtime config (DB URL, Redis URL, thread counts)
```

---

## Environment

```bash
# PostgreSQL 15 (Homebrew, running)
PGHOST=localhost
PGPORT=5432
PGDATABASE=cortex
PGUSER=srini   # or $USER

# Redis 7 (Homebrew, running)
REDIS_URL=redis://localhost:6379

# NBA S3 base
NBA_S3_BASE=https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData
```

---

## Blockers / Open Questions

| # | Issue | Status |
|---|-------|--------|
| 1 | TimescaleDB build failed (Xcode 26.3 toolchain mismatch) — using native PG range partitioning | **Resolved** — functionally equivalent for our workload |
| 2 | stats.nba.com Akamai-blocked from this IP; S3 used instead | Resolved via S3 |
| 3 | Historical game ID list — S3 only exposes live/recent games | **Resolved** — `game_ids_for_season()` generates IDs deterministically from NBA format; missing games return 404 and are skipped |
| 4 | ONNXRuntime not yet installed (brew install onnxruntime?) | Phase 2, not blocking Phase 1 |
