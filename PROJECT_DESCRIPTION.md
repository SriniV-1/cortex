# Cortex — Real-Time NBA Analytics Engine

## Handoff Document for Resume / LinkedIn

Use this document to craft resume bullet points, LinkedIn project descriptions, and portfolio entries. Everything below is factual and verified against the codebase.

---

## One-Liner

**Cortex** is a high-performance real-time NBA analytics engine built from scratch in C++20 that ingests 4.7 million play-by-play events, serves sub-20ms queries via a custom HTTP/WebSocket server, and computes live win probabilities using SIMD-accelerated similarity search and an Elo-powered ML model.

---

## Technical Summary

| Dimension | Detail |
|-----------|--------|
| **Language** | C++20 (5,369 lines production + 1,625 lines tests) |
| **Database** | PostgreSQL 15 with native range partitioning (4.7M rows across 6 time-based partitions) |
| **Data Scale** | 8,425 NBA games, 1,200 players, 30 teams, 4.7M play events (2019–2026 seasons) |
| **Data Source** | NBA's S3 public API — play-by-play, boxscores, live scoreboards |
| **ML Model** | Logistic regression via ONNXRuntime — 7-feature win probability (75.5% accuracy, 0.837 AUC) |
| **Frontend** | Single-page dashboard (961 lines HTML/CSS/JS) with real-time WebSocket updates |
| **Build System** | CMake 3.20+ targeting Ninja, 7 executables + 4 static libraries |

---

## Architecture (4 Layers)

### 1. ETL Pipeline (ingestion)
- **NBAClient**: Fetches play-by-play and boxscore JSON from NBA's S3 CDN using libcurl + simdjson parsing
- **BulkInserter**: Streams rows into PostgreSQL using the COPY binary protocol (~50K rows/sec), with per-game transactional idempotency via an `etl_progress` tracking table
- **LiveIngestor**: Background thread polls the NBA scoreboard API every 30s during live games, using per-game watermark tracking to inject only new events into the stream pipeline
- Multi-threaded season loading with early termination (stops after 30 consecutive missing game IDs)
- Supports regular season + playoffs with deterministic game ID generation

### 2. Stream Processing (real-time)
- **Lock-free SPSC ring buffer** (65,536 slots) with cache-line-padded atomics to eliminate false sharing — benchmarked at **8.7 million events/sec** on Apple Silicon
- **StatAccumulator**: Maintains per-player, per-game atomic counters (points, rebounds, assists, FG%, etc.) with rolling-window momentum calculation
- **StreamProcessor**: Consumer thread drains the ring buffer and invokes per-event callbacks for downstream subscribers (WebSocket broadcast, stat updates)

### 3. Analytics Layer (intelligence)
- **SIMD Similarity Search (GameStateIndex)**: Encodes each of 4.7M play events into 8-dimensional feature vectors (32-byte aligned), then performs brute-force L2 distance scan using **ARM NEON intrinsics** (`vld1q_f32`, `vfmaq_f32`, `vaddvq_f32`). Top-K results via min-heap with threshold pruning. **Query latency: ~6ms p99** across the full 4.7M event corpus (142 MB feature store)
- **Elo Rating System (EloTracker)**: Computes team strength from 8,400+ game results using standard Elo with K=20 (regular season), K=32 (playoffs), 100-point home court advantage, and 25% season regression to mean. Builds in ~47ms
- **Win Probability Model**: ONNX logistic regression trained on 42K game-state samples with 7 features: score differential, quarter, seconds remaining, home advantage, momentum, **Elo difference, and Elo expected win probability**. Achieves **75.5% accuracy and 0.837 AUC**. Inference via ONNXRuntime (~0.1ms per prediction)

### 4. Serving Layer (delivery)
- **Custom HTTP/1.1 + WebSocket server** built on llhttp parser + platform-native I/O multiplexing:
  - **kqueue** on macOS (edge-triggered, pipe-based wakeup)
  - **epoll** on Linux (semantically equivalent, eventfd wakeup)
  - Abstracted behind an `IOPoller` interface — compile-time platform selection
- **12 REST API endpoints**: health, metrics (Prometheus), leaderboard, recent games, player/game/event search, similarity search, Elo rankings, index status, player season stats, dashboard stats
- **WebSocket real-time broadcast**: Clients subscribe to a game ID; each play event is enriched with win probability and broadcast as JSON frames. RFC 6455 handshake with SHA-1/base64 validation
- **Redis cache-aside** (hiredis): 60s TTL on game stats, 5-minute TTL on similarity results
- **Materialized views** for instant leaderboard queries (player_game_stats pre-aggregated from 4.7M events, refreshed after each ETL load)

---

## Key Performance Metrics

| Benchmark | Result | Method |
|-----------|--------|--------|
| Ring buffer throughput | **8.7M events/sec** | SPSC lock-free with cache-line padding |
| Similarity search (4.7M vectors) | **~6ms p99** | ARM NEON SIMD brute-force L2 scan |
| Query latency (game events) | **3.2ms p99** | PostgreSQL with partition pruning |
| Query latency (player season) | **6.3ms p99** | Materialized view aggregation |
| WebSocket broadcast (1000 clients) | **15.6ms p99** | kqueue event loop + frame batching |
| Win probability inference | **~0.1ms** | ONNXRuntime single-threaded |
| Elo build (8,400 games) | **47ms** | In-memory chronological scan |

---

## Database Design

- **Range-partitioned play_events table** across 6 time-based partitions (2000–2029), enabling partition pruning for time-range queries
- **Composite primary key** (event_id, occurred_at) for partition-aligned uniqueness
- **4 covering indexes** on play_events: game lookup, player history, action type filter, time-range scan
- **Materialized view** (player_game_stats) with concurrent refresh — pre-computes box-score stats for all 1,200 players across 4.7M events
- **ON CONFLICT DO NOTHING** for fully idempotent ETL — safe to re-run any season without duplicates
- JSONB qualifiers column preserving raw NBA API data for future analytics

---

## Technologies Used

**Core**: C++20, PostgreSQL 15, Redis 7, CMake 3.20+

**Libraries**: libpqxx (PostgreSQL C++ driver), spdlog (structured logging), simdjson (SIMD JSON parsing), llhttp (HTTP parser), ONNXRuntime (ML inference), libcurl (HTTP client), hiredis (Redis client), OpenSSL (TLS/SHA-1), Google Test (unit testing)

**Systems Programming**: ARM NEON SIMD intrinsics, kqueue/epoll I/O multiplexing, lock-free data structures (SPSC queue with acquire/release atomics), cache-line padding for false sharing elimination, WebSocket RFC 6455

**ML/Analytics**: Elo rating system, logistic regression (scikit-learn → ONNX export), L2 distance brute-force with SIMD vectorization, feature engineering from play-by-play data

**Frontend**: Vanilla HTML/CSS/JS single-page app with WebSocket real-time updates, debounced search, interactive stat tabs, gradient-styled visualizations

---

## Codebase Statistics

| Component | Lines | Files |
|-----------|-------|-------|
| C++ source (.cpp) | 4,062 | 13 |
| C++ headers (.hpp) | 1,307 | 11 |
| Unit tests + benchmarks | 1,625 | 8 |
| Frontend (HTML/CSS/JS) | 961 | 1 |
| SQL schema | 228 | 1 |
| Shell + Python scripts | 593 | 3 |
| CMake build | 195 | 1 |
| **Total** | **~8,970** | **38** |

---

## Resume Bullet Points (pick 3-5)

- Built a **real-time NBA analytics engine in C++20** processing 4.7M play-by-play events with sub-20ms query latency, featuring a custom HTTP/WebSocket server, SIMD-accelerated similarity search, and Elo-powered win probability model
- Designed a **lock-free SPSC ring buffer** with cache-line-padded atomics achieving 8.7M events/sec throughput for real-time stream processing on Apple Silicon
- Implemented **ARM NEON SIMD brute-force similarity search** across 4.7M game-state vectors (142 MB feature store) with ~6ms p99 query latency using vectorized L2 distance computation
- Built a **custom HTTP/1.1 + WebSocket server** from scratch using llhttp, kqueue/epoll I/O multiplexing, and Redis cache-aside pattern, supporting 1000 concurrent WebSocket clients at 15.6ms p99 broadcast latency
- Trained an **Elo-enhanced win probability model** (7-feature logistic regression, 0.837 AUC) computed from 8,400+ game results, with real-time inference via ONNXRuntime during live NBA games
- Engineered a **PostgreSQL ETL pipeline** with range-partitioned tables, COPY protocol bulk loading (~50K rows/sec), materialized views, and idempotent upserts for 4.7M event ingestion across 7 NBA seasons
- Designed a **platform-agnostic I/O multiplexing layer** abstracting kqueue (macOS) and epoll (Linux) behind a common interface, with edge-triggered event handling and pipe/eventfd wakeup mechanisms

---

## LinkedIn Project Description (Short)

**Cortex — Real-Time NBA Analytics Engine** | C++20, PostgreSQL, Redis, ONNX, ARM NEON SIMD

Built a high-performance analytics system from scratch that ingests 4.7M NBA play-by-play events and serves real-time insights. Features include a custom HTTP/WebSocket server with kqueue/epoll I/O multiplexing (1000-client broadcast at 15.6ms p99), SIMD-accelerated similarity search across game states (~6ms p99 over 142MB feature store), an Elo rating system computing team strength from 8,400+ games, and a 7-feature win probability model achieving 0.837 AUC. Lock-free stream processing pipeline benchmarked at 8.7M events/sec. PostgreSQL schema uses range partitioning with materialized views and COPY protocol bulk loading.

---

## LinkedIn Project Description (Detailed)

**Cortex — Real-Time NBA Analytics Engine**

A full-stack analytics platform built from scratch in C++20 that processes 4.7 million NBA play-by-play events across 7 seasons (2019–2026) to deliver real-time game insights.

**Data Pipeline**: Custom ETL system fetches play-by-play data from the NBA's S3 API, parses JSON with simdjson, and bulk-loads into PostgreSQL 15 using the COPY binary protocol at ~50K rows/sec. Range-partitioned tables across 6 time-based partitions enable partition pruning for sub-millisecond queries. Fully idempotent with ON CONFLICT upserts and per-game progress tracking.

**Stream Processing**: Lock-free single-producer/single-consumer ring buffer (65,536 slots) with cache-line-padded atomics achieves 8.7M events/sec throughput. Real-time stat accumulation uses atomic counters with acquire/release memory ordering — no mutexes in the hot path.

**Analytics**: ARM NEON SIMD brute-force similarity search encodes 4.7M events into 8D feature vectors and scans the full corpus in ~6ms p99. Team Elo ratings computed from 8,400+ game results (K-factor adaptation for playoffs, season regression). Win probability model trained via scikit-learn (7 features including Elo, 0.837 AUC), exported to ONNX for ~0.1ms inference via ONNXRuntime.

**Serving**: Custom HTTP/1.1 + WebSocket server built on llhttp with kqueue (macOS) / epoll (Linux) I/O multiplexing. 12 REST endpoints with Redis cache-aside pattern. WebSocket broadcast delivers play events enriched with win probability to 1000 concurrent clients at 15.6ms p99. Single-page dashboard with real-time updates, interactive stat tabs, player/game/event search, and Elo power rankings.

**Scale**: 5,369 lines C++20, 1,625 lines tests (GTest + 4 benchmark suites), PostgreSQL 15, Redis 7, CMake build system targeting 7 executables + 4 static libraries.
