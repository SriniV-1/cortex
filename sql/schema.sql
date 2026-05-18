-- CORTEX — NBA Analytics Engine — PostgreSQL 15 Schema
-- Uses native range partitioning (no TimescaleDB required).
-- Run: psql -d cortex -f sql/schema.sql

BEGIN;

-- ── Extensions ────────────────────────────────────────────────────────────
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;

-- ── Teams ─────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS teams (
    team_id     INTEGER PRIMARY KEY,
    tricode     CHAR(3)      NOT NULL,
    full_name   VARCHAR(64)  NOT NULL,
    city        VARCHAR(64)  NOT NULL,
    conference  VARCHAR(4)   CHECK (conference IN ('East', 'West')),
    division    VARCHAR(16),
    created_at  TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX IF NOT EXISTS teams_tricode_idx ON teams (tricode);

-- ── Players ───────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS players (
    player_id   INTEGER     PRIMARY KEY,
    first_name  VARCHAR(64) NOT NULL,
    last_name   VARCHAR(64) NOT NULL,
    team_id     INTEGER     REFERENCES teams (team_id) ON DELETE SET NULL,
    jersey_num  SMALLINT,
    position    VARCHAR(8),
    is_active   BOOLEAN     NOT NULL DEFAULT true,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS players_team_idx ON players (team_id);

-- ── Games ─────────────────────────────────────────────────────────────────
-- game_id format: 10 digits, e.g. 0022300001 (season 2023-24, game 1)
CREATE TABLE IF NOT EXISTS games (
    game_id         VARCHAR(16)  PRIMARY KEY,
    season          SMALLINT     NOT NULL,        -- e.g. 2023 for 2023-24
    season_type     VARCHAR(16)  NOT NULL          -- Regular Season, Playoffs, etc.
                    CHECK (season_type IN ('Regular Season', 'Playoffs', 'Pre Season', 'All Star')),
    game_date       DATE         NOT NULL,
    home_team_id    INTEGER      NOT NULL REFERENCES teams (team_id),
    away_team_id    INTEGER      NOT NULL REFERENCES teams (team_id),
    home_score      SMALLINT,
    away_score      SMALLINT,
    status          SMALLINT     NOT NULL DEFAULT 1
                    CHECK (status IN (1, 2, 3)),   -- 1=scheduled, 2=live, 3=final
    loaded_at       TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS games_date_idx   ON games (game_date DESC);
CREATE INDEX IF NOT EXISTS games_season_idx ON games (season, season_type);
CREATE INDEX IF NOT EXISTS games_home_idx   ON games (home_team_id);
CREATE INDEX IF NOT EXISTS games_away_idx   ON games (away_team_id);

-- ── Play Events (range-partitioned by occurred_at) ────────────────────────
-- Parent table — no data lives here directly.
CREATE TABLE IF NOT EXISTS play_events (
    event_id        BIGINT       NOT NULL,          -- unique within game, assigned by ETL
    game_id         VARCHAR(16)  NOT NULL,
    action_number   INTEGER      NOT NULL,          -- NBA's action_number field
    occurred_at     TIMESTAMPTZ  NOT NULL,          -- parsed from timeActual
    period          SMALLINT     NOT NULL,
    period_type     VARCHAR(16)  NOT NULL DEFAULT 'REGULAR',
    clock           VARCHAR(16),                    -- ISO 8601 duration, e.g. PT11M45.00S
    action_type     VARCHAR(32)  NOT NULL,          -- shot, rebound, foul, etc.
    sub_type        VARCHAR(32),
    description     TEXT,
    player_id       INTEGER,                        -- NULL for team events
    team_id         INTEGER,
    x               REAL,                          -- court coordinates (-250 to 250)
    y               REAL,                          -- court coordinates (0 to 470)
    score_home      SMALLINT,
    score_away      SMALLINT,
    order_number    BIGINT,
    qualifiers      JSONB,                         -- raw array from NBA API
    PRIMARY KEY (event_id, occurred_at)
) PARTITION BY RANGE (occurred_at);

-- ── Partitions by 5-year blocks ──────────────────────────────────────────
-- Covers all modern NBA data (2000 – 2030+)
CREATE TABLE IF NOT EXISTS play_events_2000_2004
    PARTITION OF play_events
    FOR VALUES FROM ('2000-01-01') TO ('2005-01-01');

CREATE TABLE IF NOT EXISTS play_events_2005_2009
    PARTITION OF play_events
    FOR VALUES FROM ('2005-01-01') TO ('2010-01-01');

CREATE TABLE IF NOT EXISTS play_events_2010_2014
    PARTITION OF play_events
    FOR VALUES FROM ('2010-01-01') TO ('2015-01-01');

CREATE TABLE IF NOT EXISTS play_events_2015_2019
    PARTITION OF play_events
    FOR VALUES FROM ('2015-01-01') TO ('2020-01-01');

CREATE TABLE IF NOT EXISTS play_events_2020_2024
    PARTITION OF play_events
    FOR VALUES FROM ('2020-01-01') TO ('2025-01-01');

CREATE TABLE IF NOT EXISTS play_events_2025_2029
    PARTITION OF play_events
    FOR VALUES FROM ('2025-01-01') TO ('2030-01-01');

-- ── Indexes on partitioned table (propagates to all partitions) ────────────
CREATE INDEX IF NOT EXISTS play_events_game_idx
    ON play_events (game_id, action_number);

CREATE INDEX IF NOT EXISTS play_events_player_idx
    ON play_events (player_id, occurred_at DESC)
    WHERE player_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS play_events_action_idx
    ON play_events (action_type, occurred_at DESC);

CREATE INDEX IF NOT EXISTS play_events_time_idx
    ON play_events (occurred_at DESC);

-- ── ETL progress tracking ─────────────────────────────────────────────────
-- Allows the ETL pipeline to resume after interruption.
CREATE TABLE IF NOT EXISTS etl_progress (
    season      SMALLINT    NOT NULL,
    game_id     VARCHAR(16) NOT NULL,
    fetched_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    event_count INTEGER     NOT NULL DEFAULT 0,
    status      VARCHAR(16) NOT NULL DEFAULT 'done'
                CHECK (status IN ('done', 'error', 'partial')),
    error_msg   TEXT,
    PRIMARY KEY (season, game_id)
);

CREATE INDEX IF NOT EXISTS etl_progress_status_idx ON etl_progress (status);

-- ── Season metadata ───────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS seasons (
    season          SMALLINT    PRIMARY KEY,      -- e.g. 2023 for 2023-24
    season_str      VARCHAR(8)  NOT NULL,         -- e.g. "2023-24"
    start_date      DATE        NOT NULL,
    end_date        DATE,
    game_count      INTEGER,
    loaded_at       TIMESTAMPTZ
);

-- Pre-populate known seasons
INSERT INTO seasons (season, season_str, start_date, end_date) VALUES
    (1999, '1999-00', '1999-11-02', '2000-06-19'),
    (2000, '2000-01', '2000-11-01', '2001-06-15'),
    (2001, '2001-02', '2001-11-01', '2002-06-12'),
    (2002, '2002-03', '2002-10-29', '2003-06-15'),
    (2003, '2003-04', '2003-10-28', '2004-06-15'),
    (2004, '2004-05', '2004-11-02', '2005-06-23'),
    (2005, '2005-06', '2005-11-01', '2006-06-20'),
    (2006, '2006-07', '2006-10-31', '2007-06-14'),
    (2007, '2007-08', '2007-10-30', '2008-06-17'),
    (2008, '2008-09', '2008-10-28', '2009-06-14'),
    (2009, '2009-10', '2009-10-27', '2010-06-17'),
    (2010, '2010-11', '2010-10-26', '2011-06-12'),
    (2011, '2011-12', '2011-12-25', '2012-06-21'),
    (2012, '2012-13', '2012-10-30', '2013-06-20'),
    (2013, '2013-14', '2013-10-29', '2014-06-15'),
    (2014, '2014-15', '2014-10-28', '2015-06-16'),
    (2015, '2015-16', '2015-10-27', '2016-06-19'),
    (2016, '2016-17', '2016-10-25', '2017-06-12'),
    (2017, '2017-18', '2017-10-17', '2018-06-08'),
    (2018, '2018-19', '2018-10-16', '2019-06-13'),
    (2019, '2019-20', '2019-10-22', '2020-10-11'),
    (2020, '2020-21', '2020-12-22', '2021-07-20'),
    (2021, '2021-22', '2021-10-19', '2022-06-16'),
    (2022, '2022-23', '2022-10-18', '2023-06-12'),
    (2023, '2023-24', '2023-10-24', '2024-06-17'),
    (2024, '2024-25', '2024-10-22', NULL),
    (2025, '2025-26', '2025-10-21', NULL)
ON CONFLICT (season) DO NOTHING;

COMMIT;
