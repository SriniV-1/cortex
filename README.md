# Cortex — Real-Time NBA Analytics Engine

C++20 system that ingests NBA game data, computes real-time statistics, and serves live analytics via HTTP/WebSocket. Target: sub-20ms p99 query latency across a corpus of 3.7M play-by-play events (6,637 games, 2019–2025).

**Stack:** C++20 · PostgreSQL 15 · Redis 7 · ONNXRuntime · kqueue/epoll · llhttp

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
                           │
              ┌────────────┴─────────────────┐
              │ ETL Pipeline                  │ Daily Auto-Refresh
              │ NBAClient + BulkInserter      │ (4 AM, current season only)
              └────────────┬─────────────────┘
                           │ bulk COPY
                    [PostgreSQL 15]         ← range-partitioned play_events
                           │
               ┌───────────┼──────────────────────────┐
               │           │                          │
       [Ring Buffer]  [GameStateIndex]         [HTTP / WS Server]
     (SPSC, lock-free) SIMD scan, 118MB        (kqueue + llhttp)
               │        ARM NEON               │
    [Stream Processor]  3.7M vectors           GET /stats/{gameId}
    [Stat Accumulator]                         GET /players/{id}/season
    [Win Prob Model]           ────────────    GET /api/similar  ← NEON scan
      ONNX logistic            GET /live/{gameId}  ─── WebSocket + win_prob
      regression                             │
               └──── broadcast ─────────────┘
                                          │
                                    [Redis Cache]
                                   (60s TTL, /stats)
```

### Component overview

| Component | Path | Description |
|-----------|------|-------------|
| NBAClient | `src/etl/NBAClient.cpp` | Fetches game IDs and play-by-play from NBA S3 |
| BulkInserter | `src/etl/BulkInserter.cpp` | PostgreSQL COPY protocol bulk loader |
| LiveIngestor | `src/etl/LiveIngestor.cpp` | Polls NBA feed every 30s for in-progress games |
| RingBuffer | `include/stream/RingBuffer.hpp` | SPSC lock-free ring, cache-line padded |
| StreamProcessor | `src/stream/StreamProcessor.cpp` | jthread consumer, dispatches to accumulator + ONNX |
| StatAccumulator | `src/stream/StatAccumulator.cpp` | In-memory per-player/game rolling stats |
| WinProbModel | `src/analytics/WinProbModel.cpp` | ONNXRuntime logistic regression, sigmoid output |
| **GameStateIndex** | **`src/analytics/GameStateIndex.cpp`** | **SIMD nearest-neighbor search across 3.7M events** |
| KqueuePoller | `src/serving/KqueuePoller.cpp` | Edge-triggered kqueue event loop |
| HttpServer | `src/serving/HttpServer.cpp` | HTTP/1.1 + WebSocket (RFC 6455) server |
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

### 3 — Load historical data (2019–2025)

```bash
./cortex.sh load
```

Takes ~15–20 min. Loads 6,637 games / 3.7M events / 30 teams / 1,080+ players.
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
- Every 24 hours it fetches the current NBA season from the S3 feed and inserts any games not already in the database (`ON CONFLICT DO NOTHING` — safe to re-run).
- Only the active season is refreshed; historical seasons (2019–2023) are never re-fetched.
- Refresh runs entirely in the background — zero impact on HTTP latency.

**NBA season detection:** If the current month is October or later, the server treats the current calendar year as the active season start. Otherwise it uses the prior year (e.g., in March 2026 it refreshes the 2025–26 season).

---

## Win Probability

Every live play event is scored by an ONNX logistic regression model that outputs the probability the home team wins from that exact game state.

**Model inputs** (5 features):

| Feature | Description |
|---------|-------------|
| `score_diff` | Home score − away score |
| `quarter` | Current period (1–4) |
| `sec_remaining` | Total seconds remaining in game |
| `home_advantage` | 1.0 for home team |
| `momentum` | Rolling score-diff change over last 10 events |

**Where it appears:**
- WebSocket stream — each `play` event includes `"win_prob": 0.61`
- Dashboard — displayed live as plays come in during active games
- Only fires for live games (`LiveIngestor` status == 2); historical queries do not include win probability

**Model file:** `data/models/win_prob.onnx` — 207-byte ONNX graph (`MatMul → Add → Sigmoid`). If the file is missing the server degrades gracefully and broadcasts without the `win_prob` field.

---

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Service health check |
| `GET` | `/stats/{gameId}` | Live game score + event count (Redis-cached 60s) |
| `GET` | `/players/{playerId}/season` | Season aggregates (points/rebounds/etc.) |
| `GET` | `/api/leaderboard` | Top 20 players by PPG (2019–2025 corpus) |
| `GET` | `/api/games/recent` | Last 20 games with scores |
| `GET` | `/api/similar` | **SIMD similarity search** (see below) |
| `GET` | `/api/index/status` | Similarity index build status + size |
| `GET` | `/live/{gameId}` | WebSocket upgrade — streams live play events with win probability |
| `GET` | `/metrics` | Prometheus metrics (`cortex_events_processed`) |

### Similarity Search — `GET /api/similar`

Finds the K most similar historical game states from the full 3.7M-event corpus using a brute-force SIMD L2 scan (ARM NEON on Apple Silicon, scalar fallback on x86).

**Query parameters:**

| Param | Description | Default |
|-------|-------------|---------|
| `score_home` | Home team score | 50 |
| `score_away` | Away team score | 50 |
| `period` | Current quarter (1–4) | 2 |
| `clock` | Seconds remaining in period (0–720) | 360 |
| `momentum` | Score-diff change over last 10 events | 0 |
| `k` | Results to return (1–25) | 10 |

**Example:**
```
GET /api/similar?score_home=105&score_away=98&period=4&clock=180
```

**Response:**
```json
{
  "query": {"score_home": 105, "score_away": 98, "period": 4, "clock": 180, "momentum": 0},
  "query_ms": 3.84,
  "index_size": 3721440,
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
Each event is encoded as a normalized 8-float feature vector capturing score differential, time remaining, game pace, recent momentum, and closeness. At server startup the full ~118 MB feature store is loaded into RAM in a background thread (typically 15–30s). Queries scan all vectors using `vld1q_f32` + `vfmaq_f32` + `vaddvq_f32` (2 NEON loads, 1 fused multiply-accumulate per candidate), then return the top-K results from a min-heap. The dashboard auto-populates the inputs from live WebSocket events.

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
| **Similarity search, 3.7M events (p99)** | **~6 ms** | < 20 ms |

---

## Tests

```bash
cd build && ctest --output-on-failure
```

Unit tests cover: NBAClient, BulkInserter, StreamProcessor, StatAccumulator, WinProbModel, KqueuePoller, HttpServer, RedisCache.

Benchmarks:
```bash
./build/cortex_bench       # query latency (p50/p95/p99)
./build/cortex_throughput  # ring buffer throughput
./build/cortex_ws_load     # WebSocket broadcast latency (1000 clients)
./build/cortex_similarity  # SIMD similarity scan across 3.7M events
```

Or run all at once:
```bash
./cortex.sh bench
```

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

No authentication required. Data availability: 2019–present (pre-2019 returns 403).

---

## Project Structure

```
Cortex/
├── CMakeLists.txt
├── README.md
├── ROADMAP.md
├── config/
│   └── cortex.toml
├── data/
│   └── models/
│       └── win_prob.onnx        ← ONNX logistic regression (win probability)
├── include/
│   ├── common/                  ← Logger, Config
│   ├── etl/                     ← NBAClient, BulkInserter, LiveIngestor
│   ├── stream/                  ← RingBuffer, StreamProcessor, StatAccumulator, StreamEvent
│   ├── serving/                 ← IOPoller, KqueuePoller, HttpServer, RedisCache
│   └── analytics/               ← WinProbModel, GameStateIndex
├── scripts/
│   └── bootstrap_db.sh
├── sql/
│   └── schema.sql
├── src/
│   ├── etl/
│   ├── stream/
│   ├── serving/
│   └── analytics/
└── tests/
    ├── unit/
    ├── integration/
    └── benchmarks/
```
