# Cortex — Real-Time NBA Analytics Engine

## Handoff Document for Resume / LinkedIn

Use this document for resume bullets, LinkedIn descriptions and portfolio entries. Numbers below were re-checked against the code and README on 2026-09-15.

---

## One-Liner

**Cortex** is a real-time NBA analytics engine built from scratch in C++20. It ingests 4.7 million play-by-play events, serves sub-20ms queries through a hand-written HTTP/WebSocket server, finds similar historical game states with a hand-rolled HNSW index, and computes live win probabilities with an Elo-powered ONNX model.

---

## Technical Summary

| Dimension | Detail |
|-----------|--------|
| **Language** | C++20 (~10,100 lines production, ~3,800 lines tests) |
| **Database** | PostgreSQL 15 with native range partitioning (4.7M rows across 6 time-based partitions) |
| **Data Scale** | 8,400+ NBA games, 1,250 players, 30 teams, 4.7M play events (2019–2026 seasons) |
| **Data Source** | NBA's S3 public API — play-by-play, boxscores, live scoreboards |
| **ML Model** | Logistic regression via ONNX Runtime — 7-feature win probability (75.5% accuracy, 0.837 AUC) |
| **Distributed** | gRPC coordinator + N workers, consistent-hash game assignment, failure detection, epoch fencing |
| **Frontend** | Vanilla HTML/CSS/JS dashboard (~2,100 lines) with real-time WebSocket updates |
| **Deploy** | Public HTTPS demo on one AWS Graviton (arm64) EC2 instance, provisioned with Terraform, Caddy for TLS |
| **Build System** | CMake + Ninja: 9 executables (+2 optional libFuzzer targets), 6 static libraries |

---

## Architecture

### 1. ETL Pipeline (ingestion)
- **NBAClient**: fetches play-by-play and boxscore JSON from NBA's S3 CDN with libcurl, parsed by simdjson
- **BulkInserter**: streams rows into PostgreSQL with the COPY protocol (~50K rows/sec), per-game transactional idempotency via an `etl_progress` table
- **LiveIngestor**: background thread polls the NBA scoreboard during live games and injects only new events (per-game watermarks); backs `/api/scoreboard`
- Startup refresh catches up on any games missed while the server was down

### 2. Stream Processing (real-time)
- **Lock-free SPSC ring buffer** (65,536 slots) with cache-line-padded atomics — benchmarked at **8.7M events/sec**
- **StatAccumulator**: per-player, per-game atomic counters with rolling-window momentum
- **StreamProcessor**: consumer thread drains the ring buffer and fans events out to WebSocket broadcast and stat updates

### 3. Analytics Layer
- **HNSW similarity search (GameStateIndex)**: each event becomes an 8-float feature vector (142 MB feature store). Identical states are merged first (4.67M events → 2.89M unique states), then a hand-rolled HNSW graph is built (M=16, efConstruction=200). At ef=64 it reaches **0.978 recall@10 at 0.033 ms p99** on 1,000 realistic game-state queries, ~147x faster than the exact scan
- **Exact ARM NEON scan**: brute-force L2 with `vld1q_f32` / `vfmaq_f32` / `vaddvq_f32` and a top-K heap (4.3 ms p99). Serves queries while the graph builds at startup and is the ground truth for recall
- **Elo ratings (EloTracker)**: 8,400+ game results, K=20 regular season / K=32 playoffs, 100-point home advantage, 25% season regression; builds in ~47 ms; history exposed at `/api/elo/history`
- **Win probability model**: ONNX logistic regression trained on 42K game-state samples, 7 features including Elo difference and Elo expected win probability; ~0.1 ms inference

### 4. Serving Layer
- **Custom HTTP/1.1 + WebSocket server** on llhttp with kqueue (macOS) / epoll (Linux) behind an `IOPoller` interface, trie-based router
- **21 routes**: health, readiness, Prometheus metrics, stats, leaderboard, player/game/event search, recent games, live scoreboard, Elo rankings + history, similarity search, index status, per-game live stats, player season stats, WebSocket live stream, OpenAPI spec + docs page, JWT token issuance
- **WebSocket broadcast**: clients subscribe to a game; each play event is enriched with win probability. RFC 6455 handshake; per-connection outbound queue capped at 1,024 frames
- **Auth**: JWT + RBAC on non-exempt routes when a secret is configured; the public demo runs read-only without it
- **Redis cache-aside** (hiredis) on game stats and similarity results
- **OpenAPI 3.0.3 spec** served at `/api/openapi.json` with an interactive docs page at `/docs`

### 5. Distributed Mode
- **Coordinator** (gRPC) assigns live games to worker nodes over a consistent-hash ring, detects dead workers, and uses epoch fencing so a stale worker can't keep writing after reassignment
- Each worker runs its own NBAClient + RingBuffer + StreamProcessor; `docker-compose.cluster.yml` brings up a local cluster

---

## Key Performance Metrics

| Benchmark | Result | Method |
|-----------|--------|--------|
| Similarity search, HNSW ef=64 (4.7M events) | **0.033 ms p99, 0.978 recall@10** | Hand-rolled HNSW over deduplicated states |
| Similarity search, exact scan (4.7M events) | **4.3 ms p99** | ARM NEON brute-force L2 |
| Ring buffer throughput | **8.7M events/sec** | SPSC lock-free with cache-line padding |
| Query latency (game events) | **3.2 ms p99** | PostgreSQL partition pruning |
| Query latency (player season) | **6.3 ms p99** | Materialized view aggregation |
| WebSocket broadcast (1,000 clients) | **15.6 ms p99** | kqueue event loop |
| Win probability inference | **~0.1 ms** | ONNX Runtime |
| Elo build (8,400+ games) | **47 ms** | In-memory chronological scan |

---

## Testing and Hardening

- **136 tests**: 116 GoogleTest unit tests across 18 suites, 14 RapidCheck property tests, 6 integration tests against a real PostgreSQL
- **libFuzzer** harnesses for the HTTP parser and WebSocket frame decoder
- **AddressSanitizer + UBSan**: Debug builds compile with `-fsanitize=address,undefined`; CI runs the suites on Ubuntu Release, Ubuntu Debug (sanitized) and macOS Release
- Four benchmark binaries: query latency, ring buffer throughput, WebSocket load, HNSW vs exact scan

---

## Database Design

- **Range-partitioned play_events** across 6 time-based partitions (2000–2029) for partition pruning
- **Composite primary key** (event_id, occurred_at) for partition-aligned uniqueness
- **4 covering indexes**: game lookup, player history, action type filter, time-range scan
- **Materialized view** `player_game_stats` with concurrent refresh — box-score stats for all 1,250 players across 4.7M events
- **ON CONFLICT DO NOTHING** for idempotent ETL
- JSONB qualifiers column preserving raw NBA API data

---

## Technologies Used

**Core**: C++20, PostgreSQL 15, Redis 7, gRPC + Protocol Buffers, CMake

**Libraries**: libpqxx, spdlog, simdjson, llhttp, ONNX Runtime, libcurl, hiredis, OpenSSL, GoogleTest, RapidCheck, nlohmann/json

**Systems Programming**: HNSW approximate nearest neighbor search, ARM NEON SIMD intrinsics, kqueue/epoll I/O multiplexing, lock-free SPSC queue, cache-line padding, WebSocket RFC 6455, consistent hashing, epoch fencing

**ML/Analytics**: Elo ratings, logistic regression (scikit-learn → ONNX), feature engineering from play-by-play data

**Infra**: Docker, Docker Compose, Terraform, AWS EC2 Graviton, Caddy, GitHub Actions CI, Prometheus metrics

---

## Codebase Statistics (tracked files)

| Component | Lines | Files |
|-----------|-------|-------|
| C++ source (.cpp) | 7,255 | 31 |
| C++ headers (.hpp/.h) | 2,891 | 43 |
| Tests + benchmarks + fuzz | 3,766 | 19 |
| Frontend (HTML/CSS/JS) | 2,136 | 6 |
| SQL schema | 228 | 1 |
| Scripts | 376 | 2 |
| Protobuf | 80 | 1 |
| Terraform | 171 | 4 |
| CMake build | 358 | 1 |
| **Total** | **~17,260** | **108** |

---

## Resume Bullet Points

The current resume (`CS Projects/resume/resume.tex`) is the source of truth for bullet wording. Pull numbers only from the tables above.

---

## LinkedIn Project Description (Short)

**Cortex — Real-Time NBA Analytics Engine** | C++20, PostgreSQL, Redis, gRPC, ONNX Runtime

Built a real-time NBA analytics engine from scratch that ingests 4.7M play-by-play events. It runs a hand-written HTTP/WebSocket server on kqueue/epoll (1,000-client broadcast at 15.6ms p99), a hand-rolled HNSW index for game-state similarity (0.978 recall@10 at 0.033ms p99), a lock-free ingest pipeline benchmarked at 8.7M events/sec, and an Elo-enhanced win probability model served through ONNX Runtime. A gRPC coordinator shards live games across workers with failure detection and epoch fencing. Deployed as a public HTTPS demo on AWS Graviton with Terraform.

---

## LinkedIn Project Description (Detailed)

**Cortex — Real-Time NBA Analytics Engine**

A full-stack analytics platform built from scratch in C++20 that processes 4.7 million NBA play-by-play events across 7 seasons (2019–2026).

**Data Pipeline**: A custom ETL fetches play-by-play data from the NBA's S3 API, parses it with simdjson and bulk-loads PostgreSQL 15 over the COPY protocol at ~50K rows/sec. Range-partitioned tables allow partition pruning, and every load is idempotent.

**Stream Processing**: A lock-free single-producer/single-consumer ring buffer (65,536 slots) with cache-line-padded atomics handles 8.7M events/sec. Stat accumulation uses atomic counters, with no mutexes in the hot path.

**Analytics**: Game states are encoded as 8-float vectors, deduplicated from 4.67M events to 2.89M unique states, and indexed with a hand-rolled HNSW graph that answers at 0.033ms p99 with 0.978 recall@10. An exact ARM NEON scan (4.3ms p99) serves during startup and is the recall reference. Team Elo comes from 8,400+ games, and a 7-feature logistic regression (0.837 AUC) runs through ONNX Runtime in ~0.1ms.

**Serving**: A custom HTTP/1.1 + WebSocket server on llhttp with kqueue/epoll exposes 21 routes, Redis cache-aside and an OpenAPI spec. WebSocket broadcast delivers win-probability-enriched plays to 1,000 concurrent clients at 15.6ms p99.

**Distributed + Ops**: A gRPC coordinator assigns games to workers over a consistent-hash ring with failure detection and epoch fencing. 136 tests (unit, property-based, integration) plus libFuzzer harnesses run in CI, including a sanitized Debug build. The public demo runs on AWS Graviton, provisioned with Terraform.
