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
./cortex.sh load

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

---

## Architecture

```
                       S3 NBA Feed
                           │
                    [ETL Pipeline]          ← NBAClient + BulkInserter
                           │ bulk COPY
                    [PostgreSQL 15]         ← range-partitioned play_events
                           │
               ┌───────────┴──────────────┐
               │                          │
       [Ring Buffer]               [HTTP / WS Server]
     (SPSC, lock-free)           (kqueue + llhttp)
               │                          │
    [Stream Processor]          GET /stats/{gameId}
    [Stat Accumulator]          GET /players/{id}/season
    [Win Prob Model]            GET /live/{gameId}  ─── WebSocket
               │                GET /metrics        ─── Prometheus
               └──── broadcast ─┘
                                          │
                                    [Redis Cache]
                                   (60s TTL, /stats)
```

### Component overview

| Component | Path | Description |
|-----------|------|-------------|
| NBAClient | `src/etl/NBAClient.cpp` | Fetches game IDs and play-by-play from NBA S3 |
| BulkInserter | `src/etl/BulkInserter.cpp` | PostgreSQL COPY protocol bulk loader |
| RingBuffer | `include/stream/RingBuffer.hpp` | SPSC lock-free ring, cache-line padded |
| StreamProcessor | `src/stream/StreamProcessor.cpp` | jthread consumer, dispatches to accumulator + ONNX |
| StatAccumulator | `src/stream/StatAccumulator.cpp` | In-memory per-player/game rolling stats |
| WinProbModel | `src/analytics/WinProbModel.cpp` | ONNXRuntime logistic regression, sigmoid output |
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
./build/cortex_etl --seasons 2019,2020,2021,2022,2023,2024 --populate-dimensions
```

Takes ~15–20 min. Loads 6,637 games / 3.7M events / 30 teams / 1,080+ players.

### 4 — Start the server

```bash
redis-cli ping           # ensure Redis is running
./build/cortex_server    # default: port 8080, localhost:5433 DB
```

Options:
```
--port 8080
--db "host=localhost port=5433 dbname=cortex"
--redis 127.0.0.1
```

---

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Service health check |
| `GET` | `/stats/{gameId}` | Live game score + event count (Redis-cached 60s) |
| `GET` | `/players/{playerId}/season` | Season aggregates (points/rebounds/etc.) |
| `GET` | `/live/{gameId}` | WebSocket upgrade — streams live play events |
| `GET` | `/metrics` | Prometheus metrics (`cortex_events_processed`) |

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

---

## Performance Results

| Benchmark | Result | Target |
|-----------|--------|--------|
| `game_events` query (p99) | 3.2 ms | < 20 ms |
| `player_season` query (p99) | 6.3 ms | < 20 ms |
| `game_summary` query (p99) | 0.6 ms | < 20 ms |
| Ring buffer throughput | 8.7 M ev/s | > 1 M ev/s |
| WS broadcast (1000 clients, p99) | 15.6 ms | < 20 ms |

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
│       └── win_prob.onnx
├── include/
│   ├── common/          ← Logger, Config
│   ├── etl/             ← NBAClient, BulkInserter
│   ├── stream/          ← RingBuffer, StreamProcessor, StatAccumulator, StreamEvent
│   ├── serving/         ← IOPoller, KqueuePoller, HttpServer, RedisCache
│   └── analytics/       ← WinProbModel
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
