# Cortex — Real-Time NBA Analytics Engine

C++20 system that ingests NBA game data, computes real-time statistics, and serves live analytics via HTTP/WebSocket. Sub-20ms p99 query latency across 4.7M play-by-play events (8,400+ games, 2019–2026).

**Stack:** C++20 · PostgreSQL 15 · Redis 7 · ONNXRuntime · ARM NEON SIMD · kqueue/epoll · llhttp

---

## Running it (three commands)

```bash
# 1. Install dependencies (one time)
brew install cmake postgresql@15 redis libpqxx spdlog simdjson googletest \
             openssl hiredis llhttp onnxruntime

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
              +------------+------------------+
              | ETL Pipeline                  | Daily Auto-Refresh
              | NBAClient + BulkInserter      | (4 AM, current season)
              +------------+------------------+
                           | bulk COPY (~50K rows/sec)
                    [PostgreSQL 15]         <- range-partitioned play_events
                           |                   materialized views
               +-----------+---------------------------+
               |           |                           |
       [Ring Buffer]  [GameStateIndex]          [HTTP / WS Server]
     (SPSC, lock-free) SIMD scan, 142MB         (kqueue + llhttp)
               |        ARM NEON                12 REST endpoints
    [Stream Processor]  4.7M vectors             |
    [Stat Accumulator]                    GET /api/similar  <- NEON scan
    [Win Prob Model]   [Elo Tracker]      GET /api/elo     <- team rankings
      ONNX 7-feature    8,400+ games      GET /api/leaderboard
      logistic reg       K=20/32          GET /api/players/search
               |                          GET /api/games/search
               +---- broadcast ---------> GET /live/{gameId} <- WebSocket
                                           |
                                    [Redis Cache]
                                   (60s TTL, /stats)
```

### Component overview

| Component | Path | Description |
|-----------|------|-------------|
| NBAClient | `src/etl/NBAClient.cpp` | Fetches game IDs, play-by-play, and boxscores from NBA S3 |
| BulkInserter | `src/etl/BulkInserter.cpp` | PostgreSQL COPY protocol bulk loader (~50K rows/sec) |
| LiveIngestor | `src/etl/LiveIngestor.cpp` | Polls NBA feed every 30s for in-progress games |
| RingBuffer | `include/stream/RingBuffer.hpp` | SPSC lock-free ring, cache-line padded (8.7M ev/sec) |
| StreamProcessor | `src/stream/StreamProcessor.cpp` | jthread consumer, dispatches to accumulator + ONNX |
| StatAccumulator | `src/stream/StatAccumulator.cpp` | In-memory per-player/game rolling stats with time-based eviction |
| WinProbModel | `src/analytics/WinProbModel.cpp` | ONNXRuntime logistic regression, 7-feature Elo-enhanced model |
| GameStateIndex | `src/analytics/GameStateIndex.cpp` | SIMD nearest-neighbor search across 4.7M events (~6ms p99) |
| EloTracker | `src/analytics/EloTracker.cpp` | Team Elo ratings from 8,400+ game results (built in ~47ms) |
| KqueuePoller | `src/serving/KqueuePoller.cpp` | Edge-triggered kqueue event loop (epoll on Linux) |
| HttpServer | `src/serving/HttpServer.cpp` | HTTP/1.1 + WebSocket (RFC 6455), 12 endpoints |
| RedisCache | `src/serving/RedisCache.cpp` | hiredis cache-aside, graceful degradation |

---

## Quick Start

### Prerequisites (macOS arm64)

```bash
brew install cmake postgresql@15 redis libpqxx spdlog simdjson googletest openssl hiredis llhttp onnxruntime
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
--port 8080
--db "host=localhost port=5433 dbname=cortex"
--redis 127.0.0.1
--model data/models/win_prob.onnx
--live                              enable live game polling (30s interval)
--poll-interval 30000               live poll interval in milliseconds
--www www                           static file root
```

---

## Data Freshness

The server automatically keeps the database current — no manual re-runs needed after the initial load.

**How it works:**
- At server startup, a background thread schedules the first refresh for the next **4:00 AM local time**.
- Every 24 hours it fetches the current NBA season (regular season + playoffs) from the S3 feed and inserts any games not already in the database (`ON CONFLICT DO NOTHING` — safe to re-run).
- After inserting new games, the materialized view `player_game_stats` is refreshed concurrently so leaderboard queries reflect the latest data.
- Only the active season is refreshed; historical seasons are never re-fetched.
- Refresh runs entirely in the background — zero impact on HTTP latency.

**NBA season detection:** If the current month is October or later, the server treats the current calendar year as the active season start. Otherwise it uses the prior year (e.g., in March 2026 it refreshes the 2025–26 season).

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

34 tests across 10 suites: NBAClient, BulkInserter, RingBuffer, StatAccumulator, StreamProcessor, WinProbModel, KqueuePoller, HttpServer, RedisCache.

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
├── include/
│   ├── common/                     <- Logger, Config
│   ├── etl/                        <- NBAClient, BulkInserter, LiveIngestor
│   ├── stream/                     <- RingBuffer, StreamProcessor, StatAccumulator, StreamEvent
│   ├── serving/                    <- IOPoller, KqueuePoller, EpollPoller, HttpServer, RedisCache
│   └── analytics/                  <- WinProbModel, GameStateIndex, EloTracker
├── scripts/
│   ├── bootstrap_db.sh
│   └── train_win_prob.py           <- model training script (scikit-learn -> ONNX)
├── sql/
│   └── schema.sql                  <- PG15 schema with partitions + materialized views
├── src/
│   ├── etl/
│   ├── stream/
│   ├── serving/
│   └── analytics/
├── tests/
│   ├── unit/                       <- 34 gtest tests across 10 suites
│   └── benchmarks/                 <- 4 benchmark executables
└── www/
    └── index.html                  <- single-page dashboard (HTML/CSS/JS)
```
