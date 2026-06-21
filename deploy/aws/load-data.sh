#!/usr/bin/env bash
# One-time historical data load (2019–2026): ~4.7M play-by-play events across
# 8,400+ games, fetched live from the NBA S3 feed. Takes ~15–30 min.
#
# Run AFTER `docker compose -f docker-compose.prod.yml up -d` (the db must be
# running and the schema applied). Re-running is safe — the ETL is idempotent
# (ON CONFLICT DO NOTHING), so an interrupted load can simply be restarted.
#
#   cd deploy/aws && ./load-data.sh
set -euo pipefail
cd "$(dirname "$0")"

# Load .env (POSTGRES_PASSWORD)
set -a; [ -f .env ] && . ./.env; set +a
: "${POSTGRES_PASSWORD:?Set POSTGRES_PASSWORD in deploy/aws/.env}"

COMPOSE="docker compose -f docker-compose.prod.yml"
CONN="host=db port=5432 dbname=cortex user=cortex password=${POSTGRES_PASSWORD}"

# Run the cortex_etl binary inside a throwaway container on the compose network.
etl() { $COMPOSE run --rm --entrypoint ./cortex_etl cortex "$@"; }

SEASONS="2019 2020 2021 2022 2023 2024 2025"

# Single-threaded: with --threads 2, two ETL workers insert into the shared
# dimension tables (players/teams) concurrently and can acquire row locks in
# opposite order, deadlocking Postgres. One worker is plenty here (the loader
# runs once) and is deadlock-free.
THREADS=1

for y in $SEASONS; do
  echo ">> season $y — regular"
  etl --conn "$CONN" --season "$y" --threads "$THREADS"
done

for y in $SEASONS; do
  echo ">> season $y — playoffs"
  etl --conn "$CONN" --season "$y" --playoffs --threads "$THREADS"
done

echo ">> backfilling dimension tables (teams / players)"
etl --conn "$CONN" --populate-dimensions

echo
echo "Data load complete. The server keeps the current season fresh on its own."
echo "Visit https://${DOMAIN:-your-domain} to see the dashboard."
