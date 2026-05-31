#!/usr/bin/env bash
# cortex.sh — Start, stop, or check the Cortex NBA Analytics engine.
#
# Usage:
#   ./cortex.sh            → start everything and open the dashboard
#   ./cortex.sh start      → same as above
#   ./cortex.sh stop       → stop the server
#   ./cortex.sh status     → check what's running
#   ./cortex.sh load       → load historical NBA data (one-time, ~20 min)
#   ./cortex.sh bench      → run performance benchmarks

set -euo pipefail

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
PID_FILE="$SCRIPT_DIR/.cortex.pid"
LOG_FILE="$SCRIPT_DIR/.cortex.log"

PG_BIN="/opt/homebrew/opt/postgresql@15/bin"
PGPORT="${PGPORT:-5433}"
DBNAME="cortex"

# ── Colors ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'

info()    { echo -e "  ${CYAN}→${NC} $*"; }
success() { echo -e "  ${GREEN}✓${NC} $*"; }
warn()    { echo -e "  ${YELLOW}!${NC} $*"; }
err()     { echo -e "  ${RED}✗${NC} $*" >&2; }
die()     { err "$*"; exit 1; }

banner() {
  echo -e "\n${BOLD}  ╔══════════════════════════════════════╗"
  echo    "  ║   CORTEX — NBA Analytics Engine     ║"
  echo -e "  ╚══════════════════════════════════════╝${NC}\n"
}

# ── Helpers ────────────────────────────────────────────────────────────────

pg_running() {
  "$PG_BIN/pg_ctl" status -D /opt/homebrew/var/postgresql@15 &>/dev/null
}

redis_running() {
  redis-cli -p 6379 ping &>/dev/null 2>&1
}

server_running() {
  [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}

ensure_postgres() {
  if pg_running; then
    success "PostgreSQL 15 is running"
  else
    info "Starting PostgreSQL 15..."
    "$PG_BIN/pg_ctl" start \
      -D /opt/homebrew/var/postgresql@15 \
      -l /opt/homebrew/var/log/postgresql@15.log
    sleep 2
    pg_running || die "PostgreSQL failed to start. Check: $PG_BIN/pg_ctl status -D /opt/homebrew/var/postgresql@15"
    success "PostgreSQL 15 started"
  fi

  # Create DB if needed
  if ! "$PG_BIN/psql" -p "$PGPORT" -lqt 2>/dev/null | cut -d'|' -f1 | grep -qw "$DBNAME"; then
    info "Creating database '$DBNAME'..."
    "$PG_BIN/createdb" -p "$PGPORT" "$DBNAME"
    "$PG_BIN/psql" -p "$PGPORT" -d "$DBNAME" -f "$SCRIPT_DIR/sql/schema.sql" -q
    success "Database '$DBNAME' created and schema applied"
  else
    success "Database '$DBNAME' exists"
  fi
}

ensure_redis() {
  if redis_running; then
    success "Redis is running"
  else
    info "Starting Redis..."
    redis-server --daemonize yes --logfile /tmp/redis-cortex.log 2>/dev/null || true
    sleep 1
    redis_running && success "Redis started" || warn "Redis unavailable (stats caching disabled)"
  fi
}

ensure_build() {
  if [[ ! -f "$BUILD_DIR/cortex_server" ]]; then
    info "Building Cortex (first run — this takes about 30 seconds)..."
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -S "$SCRIPT_DIR" 2>&1 \
      | grep -v "^--\|^- \|^Check\|ignoring" || true
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu)" --target cortex_server 2>&1 \
      | grep -v "^--\|^- \|ignoring" || true
    [[ -f "$BUILD_DIR/cortex_server" ]] || die "Build failed. Run: cmake --build build --target cortex_server"
    success "Build complete"
  else
    success "Binary ready"
  fi
}

db_has_data() {
  local count
  count=$("$PG_BIN/psql" -p "$PGPORT" -d "$DBNAME" -tAc \
    "SELECT COUNT(*) FROM games" 2>/dev/null || echo 0)
  [[ "$count" -gt 0 ]]
}

open_browser() {
  local url="http://localhost:8080"
  sleep 1
  if command -v open &>/dev/null; then open "$url"
  elif command -v xdg-open &>/dev/null; then xdg-open "$url"
  fi
}

# ── Commands ───────────────────────────────────────────────────────────────

cmd_start() {
  banner

  if server_running; then
    local pid; pid=$(cat "$PID_FILE")
    warn "Server is already running (PID $pid)"
    echo -e "\n  ${BOLD}Dashboard:${NC} http://localhost:8080\n"
    open_browser
    return
  fi

  ensure_postgres
  ensure_redis
  ensure_build

  # Warn if no data loaded
  if ! db_has_data; then
    echo
    warn "No game data found. Run ${BOLD}./cortex.sh load${NC} to load 3.7M play events (takes ~20 min)."
    warn "The dashboard will still open but the leaderboard and games will be empty."
  fi

  info "Starting Cortex server on port 8080..."
  "$BUILD_DIR/cortex_server" \
    --port 8080 \
    --db "host=localhost port=$PGPORT dbname=$DBNAME" \
    --www "$SCRIPT_DIR/www" \
    --log "$LOG_FILE" \
    --live \
    >> "$LOG_FILE" 2>&1 &

  local pid=$!
  echo "$pid" > "$PID_FILE"

  # Wait for server to accept connections
  local tries=0
  while ! curl -sf http://localhost:8080/health &>/dev/null; do
    sleep 0.3
    tries=$((tries + 1))
    if [[ $tries -gt 30 ]]; then
      err "Server didn't start in time. Check logs: $LOG_FILE"
      rm -f "$PID_FILE"
      exit 1
    fi
  done

  echo
  success "Cortex is running! (PID $pid)"
  echo
  echo -e "  ${BOLD}Dashboard:${NC}   http://localhost:8080"
  echo -e "  ${BOLD}API:${NC}         http://localhost:8080/api/leaderboard"
  echo -e "  ${BOLD}Logs:${NC}        $LOG_FILE"
  echo -e "  ${BOLD}Stop:${NC}        ./cortex.sh stop"
  echo
  open_browser
}

cmd_stop() {
  if ! server_running; then
    warn "Server is not running"
    rm -f "$PID_FILE"
    return
  fi
  local pid; pid=$(cat "$PID_FILE")
  info "Stopping server (PID $pid)..."
  kill "$pid" 2>/dev/null || true
  rm -f "$PID_FILE"
  success "Server stopped"
}

cmd_status() {
  banner
  echo -e "  ${BOLD}Service Status${NC}\n"

  if pg_running; then
    success "PostgreSQL 15    running (port $PGPORT)"
  else
    err     "PostgreSQL 15    not running"
  fi

  if redis_running; then
    success "Redis            running (port 6379)"
  else
    warn    "Redis            not running (optional — disables caching)"
  fi

  if [[ -f "$BUILD_DIR/cortex_server" ]]; then
    success "Binary           built"
  else
    warn    "Binary           not built (run ./cortex.sh start)"
  fi

  if server_running; then
    local pid; pid=$(cat "$PID_FILE")
    success "Cortex server    running (PID $pid) → http://localhost:8080"
  else
    warn    "Cortex server    not running"
  fi

  echo
  if pg_running && db_has_data; then
    local games players
    games=$("$PG_BIN/psql" -p "$PGPORT" -d "$DBNAME" -tAc "SELECT COUNT(*) FROM games" 2>/dev/null)
    players=$("$PG_BIN/psql" -p "$PGPORT" -d "$DBNAME" -tAc "SELECT COUNT(*) FROM players" 2>/dev/null)
    echo -e "  ${DIM}Games in database:  $games${NC}"
    echo -e "  ${DIM}Players tracked:    $players${NC}"
  elif pg_running; then
    warn "Database is empty — run ./cortex.sh load to load historical data"
  fi
  echo
}

cmd_load() {
  banner
  ensure_postgres
  ensure_build

  if ! [[ -f "$BUILD_DIR/cortex_etl" ]]; then
    info "Building ETL binary..."
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu)" --target cortex_etl -q
  fi

  echo -e "  ${BOLD}Loading NBA historical data (2019–2026)${NC}"
  echo -e "  ${DIM}This fetches regular season + playoff games from the NBA S3 feed."
  echo -e "  It takes 20–30 minutes and uses about 600MB of disk.${NC}"
  echo
  read -rp "  Press Enter to start, Ctrl+C to cancel... "
  echo

  local conn="host=localhost port=$PGPORT dbname=$DBNAME"

  # Regular seasons 2019–2025
  for year in 2019 2020 2021 2022 2023 2024 2025; do
    info "Loading season $year (regular season)..."
    "$BUILD_DIR/cortex_etl" --conn "$conn" --season "$year" --threads 2
  done

  # Playoffs for all seasons (2019-present; pre-2019 returns 403 immediately)
  for year in 2019 2020 2021 2022 2023 2024 2025; do
    info "Loading season $year (playoffs)..."
    "$BUILD_DIR/cortex_etl" --conn "$conn" --season "$year" --playoffs --threads 2
  done

  info "Backfilling dimension tables (teams/players)..."
  "$BUILD_DIR/cortex_etl" --conn "$conn" --populate-dimensions

  echo
  success "Data load complete."
  echo -e "  Run ${BOLD}./cortex.sh start${NC} to launch the dashboard."
  echo
}

cmd_bench() {
  banner
  ensure_build

  if ! [[ -f "$BUILD_DIR/cortex_bench" ]]; then
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu)" \
      --target cortex_bench --target cortex_throughput \
      --target cortex_ws_load --target cortex_similarity -q
  fi

  echo -e "  ${BOLD}Query Latency Benchmark${NC}\n"
  "$BUILD_DIR/cortex_bench"

  echo -e "\n  ${BOLD}Ring Buffer Throughput${NC}\n"
  "$BUILD_DIR/cortex_throughput"

  echo -e "\n  ${BOLD}WebSocket Load Test (1000 clients)${NC}\n"
  lsof -ti tcp:19090 | xargs kill -9 2>/dev/null || true
  "$BUILD_DIR/cortex_ws_load"

  echo -e "\n  ${BOLD}Similarity Index Benchmark (SIMD scan, 3.7M events)${NC}\n"
  "$BUILD_DIR/cortex_similarity" "host=localhost port=$PGPORT dbname=$DBNAME"
}

cmd_help() {
  banner
  echo -e "  ${BOLD}Usage:${NC} ./cortex.sh [command]\n"
  echo -e "  ${GREEN}start${NC}    Start all services and open the dashboard  ${DIM}(default)${NC}"
  echo -e "  ${GREEN}stop${NC}     Stop the Cortex server"
  echo -e "  ${GREEN}status${NC}   Show what's running"
  echo -e "  ${GREEN}load${NC}     Load 6,637 games of NBA data (one-time, ~20 min)"
  echo -e "  ${GREEN}bench${NC}    Run latency and throughput benchmarks"
  echo
  echo -e "  First time? Run: ${BOLD}./cortex.sh load${NC} then ${BOLD}./cortex.sh start${NC}"
  echo
}

# ── Entrypoint ─────────────────────────────────────────────────────────────

CMD="${1:-start}"
case "$CMD" in
  start)  cmd_start  ;;
  stop)   cmd_stop   ;;
  status) cmd_status ;;
  load)   cmd_load   ;;
  bench)  cmd_bench  ;;
  help|-h|--help) cmd_help ;;
  *) err "Unknown command: $CMD"; cmd_help; exit 1 ;;
esac
