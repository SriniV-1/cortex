# Cortex — Real-Time NBA Analytics Engine

C++20 system that ingests NBA game data, computes real-time statistics, and serves live analytics via HTTP/WebSocket. Sub-20ms p99 query latency across 4.7M play-by-play events (8,400+ games, 2019–2026).

**Stack:** C++20 · PostgreSQL 15 · Redis 7 · gRPC · ONNXRuntime · ARM NEON SIMD · kqueue/epoll · llhttp

---

## Running it (three commands)

```bash
# 1. Install dependencies (one time)
brew install cmake postgresql@15 redis libpqxx spdlog simdjson googletest \
             openssl hiredis llhttp onnxruntime grpc

# 2. Load historical NBA data (one time — ~20 minutes)
cd ~/path/to/Cortex && ./cortex.sh load

# 3. Start everything and open the dashboard
./cortex.sh start
```

`./cortex.sh start` handles the rest: starts PostgreSQL and Redis if they're not running, builds the binary if needed, launches the server, and opens `http://localhost:8080` in your browser automatically.

Other commands:
```
./cortex.sh stop     — stop the server
./cortex.sh status   — show what's running
./cortex.sh bench    — run performance benchmarks
```

Once the server is running, users only need a browser — no terminal required.

---

## Architecture

```
                              S3 NBA Feed
                                  |
                 +────────────────+────────────────────+
                 |      Standalone Mode                |   Cluster Mode
                 |                                     |   (coordinator + N workers)
    +────────────+──────────────+          +───────────+──────────────+
    | ETL Pipeline              |          | Coordinator (gRPC:50051) |
    | NBAClient + BulkInserter  |          | ConsistentHashRing       |
    | Startup + Daily Refresh   |          | Failure Detection        |
    | + Game Completion Callback|          +──────┬───────────────────+
    +────────────+──────────────+                 | gRPC StreamAssignments
                 |                     +──────────+──────────+──────────+
                 |                     |          |          |          |
                 |               [Worker-1] [Worker-2] [Worker-3]  ...
                 |                each: NBAClient + RingBuffer + StreamProcessor
                 |
                 | bulk COPY (~50K rows/sec)
          [PostgreSQL 15]         <- range-partitioned play_events
                 |                   materialized views
     +-----------+---------------------------+
     |           |                           |
 [Ring Buffer]  [GameStateIndex]      [HTTP / WS Server]
 (SPSC, lock-  HNSW + brute-force     (kqueue + llhttp)
  free)         SIMD scan, 142MB      JWT auth, pagination
     |          ARM NEON              trie-based router
 [Stream Proc]  4.7M vectors          |
 [Stat Accum]                   GET /api/similar  <- NEON scan






 
 [Win Prob]    [Elo Tracker]    GET /api/elo     <- team rankings
  ONNX 7-feat   8,400+ games   GET /api/leaderboard
  logistic reg   K=20/32       GET /api/players/search
     |                         GET /api/games/search
     +---- broadcast --------> GET /live/{gameId} <- WebSocket
                                 |
                          [Redis Cache]         [Circuit Breaker]
                         (60s TTL, /stats)      (3-state: closed/open/half-open)
```

### Component overview

| Component | Path | Description |
|-----------|------|-------------|
| NBAClient | `src/etl/NBAClient.cpp` | Fetches game IDs, play-by-play, and boxscores from NBA S3 |
| BulkInserter | `src/etl/BulkInserter.cpp` | PostgreSQL COPY protocol bulk loader (~50K rows/sec) |
| LiveIngestor | `src/etl/LiveIngestor.cpp` | Polls NBA feed every 30s, game completion callback |
| RingBuffer | `include/stream/RingBuffer.hpp` | SPSC lock-free ring, cache-line padded (8.7M ev/sec) |
| StreamProcessor | `src/stream/StreamProcessor.cpp` | jthread consumer, dispatches to accumulator + ONNX |
| StatAccumulator | `src/stream/StatAccumulator.cpp` | In-memory per-player/game rolling stats with time-based eviction |
| WinProbModel | `src/analytics/WinProbModel.cpp` | ONNXRuntime logistic regression, 7-feature Elo-enhanced model |
| GameStateIndex | `src/analytics/GameStateIndex.cpp` | SIMD nearest-neighbor search across 4.7M events (~6ms p99) |
| HNSWIndex | `src/analytics/HNSWIndex.cpp` | Hand-rolled HNSW graph, >95% recall@10, <1ms queries |
| EloTracker | `src/analytics/EloTracker.cpp` | Team Elo ratings from 8,400+ game results (built in ~47ms) |
| Router | `src/serving/Router.cpp` | Trie-based URL router with path params and middleware |
| KqueuePoller | `src/serving/KqueuePoller.cpp` | Edge-triggered kqueue event loop (epoll on Linux) |
| HttpServer | `src/serving/HttpServer.cpp` | HTTP/1.1 + WebSocket (RFC 6455), JWT auth, pagination |
| RedisCache | `src/serving/RedisCache.cpp` | hiredis cache-aside with circuit breaker |
| Coordinator | `src/distributed/Coordinator.cpp` | gRPC cluster brain: game assignment, failure detection, epoch fencing |
| IngestorNode | `src/distributed/IngestorNode.cpp` | Worker node: per-game poll threads, SPSC ring buffer, heartbeat |
| ConsistentHashRing | `src/distributed/ConsistentHashRing.cpp` | FNV-1a hash ring with 150 virtual nodes, minimal key migration |

---

## Quick Start

### Prerequisites (macOS arm64)

```bash
brew install cmake postgresql@15 redis libpqxx spdlog simdjson googletest openssl hiredis llhttp onnxruntime grpc
```

### 1 — Bootstrap the database

```bash
./scripts/bootstrap_db.sh          # creates cortex DB and applies sql/schema.sql
```

### 2 — Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.logicalcpu)
```

### 3 — Load historical data (2019–2026)

```bash
./cortex.sh load
```

Takes ~15–20 min. Loads 8,400+ games / 4.7M events / 30 teams / 1,250 players.
After the initial load, the server keeps data current automatically — see [Data Freshness](#data-freshness).

### 4 — Start the server

```bash
./cortex.sh start
# or directly:
./build/cortex_server --port 8080 --db "host=localhost port=5433 dbname=cortex" --live
```

Options:
```
--port 8080                         HTTP server port
--db "host=localhost port=5433 dbname=cortex"
--redis 127.0.0.1                   Redis host
--model data/models/win_prob.onnx   ONNX model path
--live                              enable live game polling (30s interval)
--poll-interval 30000               live poll interval in milliseconds
--www www                           static file root
--mode standalone|coordinator|worker server mode (default: standalone)
--grpc-port 50051                   gRPC port (coordinator mode)
--coordinator host:50051            coordinator address (worker mode)
--capacity 20                       max games per worker (worker mode)
```

---

## Data Freshness

The server automatically keeps the database current — no manual re-runs needed after the initial load.

**Three layers of automatic ingestion:**

1. **Startup refresh** — On every server start, a background thread immediately scans the current NBA season (regular + playoffs) for any games not yet in the database. New games go through the full ETL pipeline (boxscore for teams/players/game metadata, then play-by-play via COPY). The materialized view and Elo ratings are rebuilt if new data is found. This means restarting the server always catches up on missed games.

2. **Game completion callback** — When `--live` is enabled, the LiveIngestor polls the NBA scoreboard every 30 seconds. If a game it was tracking live (status=2) transitions to final (status=3), it immediately persists the completed game to the database, refreshes the materialized view, and rebuilds Elo ratings. Games appear in the dashboard within one poll cycle (~30 seconds) of the final buzzer.

3. **Daily full refresh** — At 4:00 AM local time each day, a background thread scans the full season to catch any games that were missed (e.g., server was off, or games completed while live tracking wasn't active). Uses `ON CONFLICT DO NOTHING` so re-runs are safe.

**NBA season detection:** If the current month is October or later, the server treats the current calendar year as the active season start. Otherwise it uses the prior year (e.g., in March 2026 it refreshes the 2025–26 season).

All refresh operations run entirely in the background — zero impact on HTTP latency.

---

## Distributed Cluster

Cortex can run as a distributed stream processing cluster, modeled after Kafka consumer groups. The distribution is load-bearing — multiple worker nodes coordinate game ownership, detect failures, and hand off state.

### Architecture

- **Coordinator** — single leader process that tracks cluster membership via gRPC, assigns games to workers using a consistent hash ring, detects worker failure (3 missed heartbeats = 6 seconds), and triggers automatic reassignment with epoch-based fencing tokens to prevent split-brain.
- **Workers (IngestorNode)** — each owns a subset of live games, runs its own `NBAClient` + `RingBuffer` + `StreamProcessor` pipeline for assigned games. Workers register on startup, send heartbeats every 2 seconds, and listen for assignment/revoke streams.
- **Consistent Hash Ring** — FNV-1a 64-bit hash with 150 virtual nodes per physical node. Adding a worker migrates only ~1/N of games (not a full reshuffle). Removing a worker reassigns only its games to ring successors.

### Running a cluster

```bash
# Using Docker Compose (1 coordinator + 3 workers + Postgres + Redis)
docker compose -f docker-compose.cluster.yml up --build

# Or manually:
# Terminal 1: Coordinator
./build/cortex_server --mode coordinator --grpc-port 50051 --port 8080 --db "..."

# Terminal 2-4: Workers
./build/cortex_server --mode worker --coordinator localhost:50051 --port 8081 --capacity 20
./build/cortex_server --mode worker --coordinator localhost:50051 --port 8082 --capacity 20
./build/cortex_server --mode worker --coordinator localhost:50051 --port 8083 --capacity 20
```

### Failure recovery

When a worker dies, the coordinator detects the failure within 6 seconds and reassigns its games to surviving workers. The new owner replays the full play-by-play from NBA S3 (stateless recovery — ~500 events, <1 second) to rebuild accumulator state. Epoch fencing ensures a revived worker cannot operate on stale assignments.

Standalone mode (`--mode standalone`, the default) preserves the original single-process behavior with zero changes.

---

## Elo Rating System

Every team has an Elo rating computed from 8,400+ historical game results. Ratings are built in ~47ms at server startup and persisted to the `team_elo` database table.

**Parameters:**
| Parameter | Value |
|-----------|-------|
| Starting rating | 1500 |
| K-factor (regular season) | 20 |
| K-factor (playoffs) | 32 |
| Home court advantage | +100 points |
| Season regression | 25% toward 1500 at season boundary |

**Expected score formula:**
```
E = 1 / (1 + 10^(-(Elo_home + 100 - Elo_away) / 400))
```

**Where it appears:**
- `GET /api/elo` — all 30 teams ranked by Elo
- Win probability model — `elo_diff` and `elo_expected` are the two strongest features
- Dashboard — Team Power Rankings section with expandable cards showing calculation details
- Game State Search — combined probability weights 60% historical similarity + 40% Elo

---

## Win Probability

Every live play event is scored by an ONNX logistic regression model that outputs the probability the home team wins from that exact game state.

**Model inputs** (7 features, 75.5% accuracy, 0.837 AUC):

| Feature | Description |
|---------|-------------|
| `score_diff` | Home score - away score |
| `quarter` | Current period (1-4, 5+ for OT) |
| `sec_remaining` | Total seconds remaining in game |
| `home_advantage` | 1.0 for home team |
| `momentum` | Rolling score-diff change over last 10 events |
| `elo_diff` | Home Elo rating - away Elo rating |
| `elo_expected` | Elo expected win probability for home team |

**Training:** scikit-learn logistic regression on 42K game-state samples, exported to ONNX (217 bytes). `elo_expected` is the strongest predictor (coefficient +1.34).

**Where it appears:**
- WebSocket stream — each `play` event includes `"win_prob": 0.61`
- Dashboard — displayed live as plays come in during active games
- Game State Search — combined with historical similarity results

**Model file:** `data/models/win_prob.onnx` — 217-byte ONNX graph (`MatMul -> Add -> Sigmoid`). If the file is missing the server degrades gracefully and broadcasts without the `win_prob` field.

---

## Dashboard

The single-page dashboard at `http://localhost:8080` provides:

- **Stat chips** — total games, play events, players tracked, live processed count
- **Leaderboard** — top players by PPG/RPG/SPG/BPG/FG%/FT% with tab switching and player search (debounced API calls)
- **Recent Games** — filterable by All/Regular/Playoffs with team search
- **Live Stream** — auto-subscribes to live NBA games via WebSocket; click any live game to switch subscription
- **Game State Search** — enter a game scenario (scores, quarter, clock, teams) to find the 10 closest historical matches via SIMD vector search. Team inputs have autofill dropdown ranked by Elo. Shows combined win probability (60% historical + 40% Elo).
- **Team Power Rankings** — all 30 teams ranked by Elo. Click any team to open a modal showing detailed stats (rating, delta from 1500, win %, games played) and a full explanation of how Elo is calculated.

---

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Service health check |
| `GET` | `/metrics` | Prometheus metrics (`cortex_events_processed`) |
| `GET` | `/api/stats` | Dashboard stats (total games, events, players) |
| `GET` | `/api/leaderboard?stat=ppg` | Top 20 players by stat (ppg/rpg/spg/bpg/fg_pct/ft_pct) |
| `GET` | `/api/games/recent?type=playoffs` | Last 20 games, filterable by regular/playoffs |
| `GET` | `/api/players/search?q=LeBron` | Player search with full stat line |
| `GET` | `/api/games/search?team=BOS` | Game search by team tricode |
| `GET` | `/api/events/search?player_id=&action_type=` | Play event search with whitelist validation |
| `GET` | `/api/similar` | SIMD similarity search (see below) |
| `GET` | `/api/elo` | Team Elo ratings ranked by strength |
| `GET` | `/api/index/status` | Similarity index build status + size |
| `GET` | `/stats/{gameId}` | Live game score + event count (Redis-cached 60s) |
| `GET` | `/players/{playerId}/season` | Player season aggregates |
| `GET` | `/live/{gameId}` | WebSocket upgrade — streams live play events with win probability |

### Similarity Search — `GET /api/similar`

Finds the K most similar historical game states from the full 4.7M-event corpus using a brute-force SIMD L2 scan (ARM NEON on Apple Silicon, scalar fallback on x86).

**Query parameters:**

| Param | Description | Default |
|-------|-------------|---------|
| `score_home` | Home team score | 50 |
| `score_away` | Away team score | 50 |
| `period` | Current quarter (1-4) | 2 |
| `clock` | Seconds remaining in period (0-720) | 360 |
| `momentum` | Score-diff change over last 10 events | 0 |
| `k` | Results to return (1-25) | 10 |

**Example:**
```
GET /api/similar?score_home=105&score_away=98&period=4&clock=180
```

**Response:**
```json
{
  "query": {"score_home": 105, "score_away": 98, "period": 4, "clock": 180, "momentum": 0},
  "query_ms": 3.84,
  "index_size": 4666728,
  "results": [
    {
      "rank": 1,
      "game_id": "0022301234",
      "home": "MIL", "away": "BOS",
      "date": "2024-01-15",
      "score_home": 106, "score_away": 99,
      "period": 4,
      "similarity": 0.9712,
      "home_won": true
    }
  ]
}
```

**How it works:**
Each event is encoded as a normalized 8-float feature vector capturing score differential, time remaining, game pace, recent momentum, and closeness. At server startup the full ~142 MB feature store is loaded into RAM in a background thread (typically 15-30s). Queries scan all vectors using `vld1q_f32` + `vfmaq_f32` + `vaddvq_f32` (2 NEON loads, 1 fused multiply-accumulate per candidate), then return the top-K results from a min-heap. The dashboard auto-populates the inputs from live WebSocket events.

### WebSocket event format

```json
{
  "event": "play",
  "game_id": "0022300001",
  "player_id": 203507,
  "action": 1,
  "shot_made": true,
  "score_home": 42,
  "score_away": 38,
  "win_prob": 0.61
}
```

`win_prob` is omitted if the ONNX model is unavailable. All other fields are always present.

---

## Performance Results

| Benchmark | Result | Target |
|-----------|--------|--------|
| `game_events` query (p99) | 3.2 ms | < 20 ms |
| `player_season` query (p99) | 6.3 ms | < 20 ms |
| `game_summary` query (p99) | 0.6 ms | < 20 ms |
| Ring buffer throughput | 8.7 M ev/s | > 1 M ev/s |
| WS broadcast (1000 clients, p99) | 15.6 ms | < 20 ms |
| Similarity search (4.7M events, p99) | ~6 ms | < 20 ms |
| Win probability inference | ~0.1 ms | — |
| Elo build (8,400+ games) | 47 ms | — |

---

## Memory Management

The server is designed for long-running deployment with no memory leaks:

- **StatAccumulator eviction** — a background thread runs every 10 minutes and evicts in-memory stat entries for games with no activity in the last 4 hours
- **WebSocket backpressure** — per-connection outbound frame queue capped at 1024 frames; slow clients that exceed the cap are disconnected
- **Graceful shutdown** — all background threads use interruptible sleep (1-second tick checking `stop_requested()`); explicit `request_stop()` + `join()` in controlled order before locals destruct
- **AddressSanitizer clean** — all 29 tests pass under ASan with zero errors

---

## Tests

```bash
cd build && ctest --output-on-failure
```

43+ tests across 12 suites: NBAClient, BulkInserter, RingBuffer, StatAccumulator, StreamProcessor, WinProbModel, KqueuePoller, HttpServer, RedisCache, Router, ConsistentHashRing, and more. Plus property-based tests (RapidCheck) and fuzz harnesses (libFuzzer).

Benchmarks:
```bash
./build/cortex_bench       # query latency (p50/p95/p99)
./build/cortex_throughput  # ring buffer throughput
./build/cortex_ws_load     # WebSocket broadcast latency (1000 clients)
./build/cortex_similarity  # SIMD similarity scan across 4.7M events
```

Or run all at once:
```bash
./cortex.sh bench
```

---

## Database Design

- **Range-partitioned `play_events`** across 6 time-based partitions (2000-2029), enabling partition pruning for time-range queries
- **Composite primary key** `(event_id, occurred_at)` for partition-aligned uniqueness
- **4 covering indexes**: game lookup, player history, action type filter, time-range scan
- **Materialized view** `player_game_stats` with concurrent refresh — pre-computes box-score stats for all 1,250 players across 4.7M events
- **`team_elo` table** — Elo ratings persisted after each build
- **`ON CONFLICT DO NOTHING`** for fully idempotent ETL — safe to re-run any season
- **JSONB qualifiers column** preserving raw NBA API data for future analytics

---

## Configuration

`config/cortex.toml` — runtime configuration:

```toml
[database]
host     = "localhost"
port     = 5433
dbname   = "cortex"
user     = ""          # empty = $USER

[redis]
url      = "redis://localhost:6379"
ttl_secs = 60

[server]
host = "0.0.0.0"
port = 8080
```

---

## Data Source

NBA play-by-play data is fetched from the public NBA S3 endpoint:
```
https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData/
```

No authentication required. Data availability: 2019-present (pre-2019 returns 403).

---

## Project Structure

```
Cortex/
├── CMakeLists.txt
├── README.md
├── ROADMAP.md
├── PROJECT_DESCRIPTION.md
├── cortex.sh                       <- start/stop/load/bench convenience script
├── config/
│   └── cortex.toml
├── data/
│   └── models/
│       └── win_prob.onnx           <- ONNX logistic regression (7-feature, 217 bytes)
├── proto/
│   └── cortex.proto                <- gRPC service definition (coordinator-worker RPCs)
├── include/
│   ├── common/                     <- Logger, Config
│   ├── etl/                        <- NBAClient, BulkInserter, LiveIngestor
│   ├── stream/                     <- RingBuffer, StreamProcessor, StatAccumulator, StreamEvent
│   ├── serving/                    <- IOPoller, KqueuePoller, EpollPoller, HttpServer, Router, RedisCache
│   ├── analytics/                  <- WinProbModel, GameStateIndex, EloTracker, HNSWIndex
│   └── distributed/               <- Coordinator, IngestorNode, ConsistentHashRing
├── scripts/
│   ├── bootstrap_db.sh
│   └── train_win_prob.py           <- model training script (scikit-learn -> ONNX)
├── sql/
│   └── schema.sql                  <- PG15 schema with partitions + materialized views
├── src/
│   ├── etl/
│   ├── stream/
│   ├── serving/                    <- handlers/ subdirectory for route handlers
│   ├── analytics/
│   └── distributed/
├── tests/
│   ├── unit/                       <- 43+ gtest tests across 12 suites
│   ├── property/                   <- RapidCheck property-based tests
│   ├── fuzz/                       <- libFuzzer harnesses (HTTP parser, WS frames)
│   ├── integration/                <- end-to-end tests (DB, API, Elo)
│   └── benchmarks/                 <- 5 benchmark executables
└── www/
    └── index.html                  <- single-page dashboard (HTML/CSS/JS)
```
