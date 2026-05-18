#!/usr/bin/env bash
# bootstrap_db.sh — Create the cortex database and apply schema
# Usage: ./scripts/bootstrap_db.sh [--port 5433]

set -euo pipefail

PGPORT="${PGPORT:-5433}"
DBNAME="cortex"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCHEMA="$SCRIPT_DIR/../sql/schema.sql"

PG_BIN="/opt/homebrew/opt/postgresql@15/bin"

echo "==> Using PostgreSQL at $PG_BIN (port $PGPORT)"

# Start PG15 if not running
if ! "$PG_BIN/pg_ctl" status -D /opt/homebrew/var/postgresql@15 &>/dev/null; then
    echo "==> Starting PostgreSQL 15 on port $PGPORT..."
    "$PG_BIN/pg_ctl" start \
        -D /opt/homebrew/var/postgresql@15 \
        -l /opt/homebrew/var/log/postgresql@15.log
    sleep 2
fi

# Create database (idempotent)
if ! "$PG_BIN/psql" -p "$PGPORT" -lqt | cut -d'|' -f1 | grep -qw "$DBNAME"; then
    echo "==> Creating database '$DBNAME'..."
    "$PG_BIN/createdb" -p "$PGPORT" "$DBNAME"
else
    echo "==> Database '$DBNAME' already exists."
fi

# Apply schema
echo "==> Applying schema..."
"$PG_BIN/psql" -p "$PGPORT" -d "$DBNAME" -f "$SCHEMA"

echo "==> Done. Connect with:"
echo "    psql -p $PGPORT -d $DBNAME"
