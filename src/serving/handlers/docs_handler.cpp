#include "serving/handlers/docs_handler.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cortex::serving::handlers {

// ── OpenAPI 3.0.3 specification ─────────────────────────────────────────────

static json build_openapi_spec() {
    json spec;

    // ── Top-level metadata ──────────────────────────────────────────────────
    spec["openapi"] = "3.0.3";
    spec["info"] = {
        {"title",       "Cortex NBA Analytics API"},
        {"description", "Real-time NBA analytics engine with Elo ratings, "
                        "game-state similarity search, live scoreboards, and "
                        "comprehensive player/game statistics."},
        {"version",     "0.1.0"}
    };
    spec["servers"] = json::array({{{"url", "/"}}});

    // ── Reusable components ─────────────────────────────────────────────────
    json& schemas = spec["components"]["schemas"];

    schemas["Error"] = {
        {"type", "object"},
        {"properties", {
            {"error", {{"type", "string"}}}
        }},
        {"required", json::array({"error"})}
    };

    schemas["PaginatedMeta"] = {
        {"type", "object"},
        {"properties", {
            {"has_more",    {{"type", "boolean"}}},
            {"next_cursor", {{"type", "string"}}}
        }}
    };

    schemas["HealthCheck"] = {
        {"type", "object"},
        {"properties", {
            {"status", {{"type", "string"}, {"enum", json::array({"healthy", "degraded"})}}},
            {"checks", {{"type", "object"}, {"additionalProperties", true}}},
            {"circuit_breakers", {{"type", "object"}, {"additionalProperties", true}}}
        }}
    };

    schemas["ReadyCheck"] = {
        {"type", "object"},
        {"properties", {
            {"ready",            {{"type", "boolean"}}},
            {"database",         {{"type", "string"}}},
            {"similarity_index", {{"type", "string"}}}
        }}
    };

    schemas["GameStats"] = {
        {"type", "object"},
        {"properties", {
            {"game_id",          {{"type", "string"}}},
            {"score_home",       {{"type", "integer"}}},
            {"score_away",       {{"type", "integer"}}},
            {"events_processed", {{"type", "integer"}}}
        }}
    };

    schemas["PlayerSeason"] = {
        {"type", "object"},
        {"properties", {
            {"player_id", {{"type", "integer"}}},
            {"games",     {{"type", "integer"}}},
            {"points",    {{"type", "integer"}}},
            {"rebounds",  {{"type", "integer"}}},
            {"assists",   {{"type", "integer"}}},
            {"steals",    {{"type", "integer"}}},
            {"blocks",    {{"type", "integer"}}},
            {"turnovers", {{"type", "integer"}}},
            {"fga",       {{"type", "integer"}}},
            {"fgm",       {{"type", "integer"}}},
            {"fta",       {{"type", "integer"}}},
            {"ftm",       {{"type", "integer"}}}
        }}
    };

    schemas["GameSummary"] = {
        {"type", "object"},
        {"properties", {
            {"game_id",    {{"type", "string"}}},
            {"date",       {{"type", "string"}}},
            {"home",       {{"type", "string"}}},
            {"away",       {{"type", "string"}}},
            {"home_name",  {{"type", "string"}}},
            {"away_name",  {{"type", "string"}}},
            {"home_score", {{"type", "integer"}}},
            {"away_score", {{"type", "integer"}}},
            {"status",     {{"type", "integer"}}}
        }}
    };

    schemas["PlayerSearchResult"] = {
        {"type", "object"},
        {"properties", {
            {"player_id", {{"type", "integer"}}},
            {"name",      {{"type", "string"}}},
            {"team",      {{"type", "string"}}},
            {"pos",       {{"type", "string"}}},
            {"games",     {{"type", "integer"}}},
            {"pts",       {{"type", "integer"}}},
            {"reb",       {{"type", "integer"}}},
            {"stl",       {{"type", "integer"}}},
            {"blk",       {{"type", "integer"}}},
            {"ppg",       {{"type", "number"}}},
            {"rpg",       {{"type", "number"}}},
            {"spg",       {{"type", "number"}}},
            {"bpg",       {{"type", "number"}}},
            {"fg_pct",    {{"type", "number"}}},
            {"ft_pct",    {{"type", "number"}}}
        }}
    };

    schemas["GameSearchResult"] = {
        {"type", "object"},
        {"properties", {
            {"game_id",     {{"type", "string"}}},
            {"date",        {{"type", "string"}}},
            {"home",        {{"type", "string"}}},
            {"away",        {{"type", "string"}}},
            {"home_name",   {{"type", "string"}}},
            {"away_name",   {{"type", "string"}}},
            {"home_score",  {{"type", "integer"}}},
            {"away_score",  {{"type", "integer"}}},
            {"status",      {{"type", "integer"}}},
            {"season_type", {{"type", "string"}}}
        }}
    };

    schemas["EventSearchResult"] = {
        {"type", "object"},
        {"properties", {
            {"event_id",    {{"type", "integer"}}},
            {"game_id",     {{"type", "string"}}},
            {"action_num",  {{"type", "integer"}}},
            {"time",        {{"type", "string"}}},
            {"period",      {{"type", "integer"}}},
            {"clock",       {{"type", "string"}}},
            {"action",      {{"type", "string"}}},
            {"sub_type",    {{"type", "string"}}},
            {"desc",        {{"type", "string"}}},
            {"player_id",   {{"type", "integer"}, {"nullable", true}}},
            {"player_name", {{"type", "string"}}},
            {"score_home",  {{"type", "integer"}}},
            {"score_away",  {{"type", "integer"}}}
        }}
    };

    schemas["EloTeam"] = {
        {"type", "object"},
        {"properties", {
            {"rank",    {{"type", "integer"}}},
            {"team_id", {{"type", "integer"}}},
            {"tricode", {{"type", "string"}}},
            {"rating",  {{"type", "integer"}}},
            {"games",   {{"type", "integer"}}},
            {"wins",    {{"type", "integer"}}},
            {"losses",  {{"type", "integer"}}}
        }}
    };

    schemas["EloSnapshot"] = {
        {"type", "object"},
        {"properties", {
            {"season",  {{"type", "string"}}},
            {"tricode", {{"type", "string"}}},
            {"rating",  {{"type", "integer"}}}
        }}
    };

    schemas["SimilarResult"] = {
        {"type", "object"},
        {"properties", {
            {"rank",       {{"type", "integer"}}},
            {"event_id",   {{"type", "integer"}}},
            {"game_id",    {{"type", "string"}}},
            {"home",       {{"type", "string"}}},
            {"away",       {{"type", "string"}}},
            {"date",       {{"type", "string"}}},
            {"score_home", {{"type", "integer"}}},
            {"score_away", {{"type", "integer"}}},
            {"period",     {{"type", "integer"}}},
            {"home_won",   {{"type", "boolean"}}},
            {"similarity", {{"type", "number"}}}
        }}
    };

    schemas["ScoreboardGame"] = {
        {"type", "object"},
        {"properties", {
            {"game_id",    {{"type", "string"}}},
            {"status",     {{"type", "string"}}},
            {"home",       {{"type", "string"}}},
            {"away",       {{"type", "string"}}},
            {"home_score", {{"type", "integer"}}},
            {"away_score", {{"type", "integer"}}},
            {"period",     {{"type", "integer"}}},
            {"game_clock", {{"type", "string"}}}
        }}
    };

    schemas["LeaderboardPlayer"] = {
        {"type", "object"},
        {"properties", {
            {"rank",      {{"type", "integer"}}},
            {"player_id", {{"type", "integer"}}},
            {"name",      {{"type", "string"}}},
            {"team",      {{"type", "string"}}},
            {"pos",       {{"type", "string"}}},
            {"games",     {{"type", "integer"}}},
            {"pts",       {{"type", "integer"}}},
            {"reb",       {{"type", "integer"}}},
            {"stl",       {{"type", "integer"}}},
            {"blk",       {{"type", "integer"}}},
            {"ppg",       {{"type", "number"}}},
            {"rpg",       {{"type", "number"}}},
            {"spg",       {{"type", "number"}}},
            {"bpg",       {{"type", "number"}}},
            {"fg_pct",    {{"type", "number"}}},
            {"ft_pct",    {{"type", "number"}}}
        }}
    };

    schemas["DbStats"] = {
        {"type", "object"},
        {"properties", {
            {"total_games",   {{"type", "integer"}}},
            {"total_players", {{"type", "integer"}}},
            {"total_events",  {{"type", "integer"}}}
        }}
    };

    schemas["IndexStatus"] = {
        {"type", "object"},
        {"properties", {
            {"loaded",   {{"type", "boolean"}}},
            {"size",     {{"type", "integer"}}},
            {"build_ms", {{"type", "integer"}}}
        }}
    };

    // ── Reusable parameters ─────────────────────────────────────────────────
    json& params = spec["components"]["parameters"];

    params["LimitParam"] = {
        {"name", "limit"},
        {"in", "query"},
        {"description", "Maximum number of items to return (default 25, max 100)"},
        {"schema", {{"type", "integer"}, {"default", 25}, {"minimum", 1}, {"maximum", 100}}}
    };

    params["CursorParam"] = {
        {"name", "cursor"},
        {"in", "query"},
        {"description", "Opaque cursor for pagination (base64-encoded JSON)"},
        {"schema", {{"type", "string"}}}
    };

    // ── Paths ───────────────────────────────────────────────────────────────
    json& paths = spec["paths"];

    // GET /health
    paths["/health"]["get"] = {
        {"tags", json::array({"Infrastructure"})},
        {"summary", "Health check"},
        {"description", "Returns overall system health and status of subsystems "
                        "(database, Redis, similarity index, Elo tracker)."},
        {"operationId", "getHealth"},
        {"responses", {
            {"200", {
                {"description", "All subsystems healthy"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/HealthCheck"}}}}}}}
            }},
            {"503", {
                {"description", "One or more subsystems degraded"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/HealthCheck"}}}}}}}
            }}
        }}
    };

    // GET /ready
    paths["/ready"]["get"] = {
        {"tags", json::array({"Infrastructure"})},
        {"summary", "Readiness probe"},
        {"description", "Returns whether the server is ready to serve traffic "
                        "(database connected, similarity index loaded)."},
        {"operationId", "getReady"},
        {"responses", {
            {"200", {
                {"description", "Server is ready"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/ReadyCheck"}}}}}}}
            }},
            {"503", {
                {"description", "Server is not ready"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/ReadyCheck"}}}}}}}
            }}
        }}
    };

    // GET /metrics
    paths["/metrics"]["get"] = {
        {"tags", json::array({"Infrastructure"})},
        {"summary", "Prometheus metrics"},
        {"description", "Returns server metrics in Prometheus exposition format."},
        {"operationId", "getMetrics"},
        {"responses", {
            {"200", {
                {"description", "Prometheus-formatted metrics"},
                {"content", {{"text/plain; version=0.0.4", {{"schema", {{"type", "string"}}}}}}}
            }}
        }}
    };

    // GET /stats/{gameId}
    paths["/stats/{gameId}"]["get"] = {
        {"tags", json::array({"Games"})},
        {"summary", "Live game stats from stream accumulator"},
        {"description", "Returns current accumulated score and event count for a game "
                        "being tracked by the stream processor."},
        {"operationId", "getGameStats"},
        {"parameters", json::array({
            {{"name", "gameId"}, {"in", "path"}, {"required", true},
             {"description", "NBA game ID (e.g., 0022300001)"},
             {"schema", {{"type", "string"}}}}
        })},
        {"responses", {
            {"200", {
                {"description", "Game stats"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/GameStats"}}}}}}}
            }},
            {"400", {
                {"description", "Missing game_id"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }},
            {"500", {
                {"description", "Internal error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /players/{playerId}/season
    paths["/players/{playerId}/season"]["get"] = {
        {"tags", json::array({"Players"})},
        {"summary", "Player season totals"},
        {"description", "Returns aggregated season statistics for a player."},
        {"operationId", "getPlayerSeason"},
        {"parameters", json::array({
            {{"name", "playerId"}, {"in", "path"}, {"required", true},
             {"description", "Player ID"},
             {"schema", {{"type", "integer"}}}}
        })},
        {"responses", {
            {"200", {
                {"description", "Player season stats"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/PlayerSeason"}}}}}}}
            }},
            {"400", {
                {"description", "Invalid player_id"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }},
            {"404", {
                {"description", "Player not found"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/stats
    paths["/api/stats"]["get"] = {
        {"tags", json::array({"Stats"})},
        {"summary", "Database statistics"},
        {"description", "Returns total counts of games, players, and play events in the database."},
        {"operationId", "getDbStats"},
        {"responses", {
            {"200", {
                {"description", "Database counts"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/DbStats"}}}}}}}
            }},
            {"500", {
                {"description", "Database error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/leaderboard
    paths["/api/leaderboard"]["get"] = {
        {"tags", json::array({"Stats"})},
        {"summary", "Statistical leaderboard"},
        {"description", "Returns the top 50 players by a chosen stat, filtered to "
                        "players with at least 30 games played."},
        {"operationId", "getLeaderboard"},
        {"parameters", json::array({
            {{"name", "stat"}, {"in", "query"},
             {"description", "Stat to sort by"},
             {"schema", {{"type", "string"}, {"default", "ppg"},
                         {"enum", json::array({"ppg", "rpg", "spg", "bpg", "fg_pct", "ft_pct"})}}}}
        })},
        {"responses", {
            {"200", {
                {"description", "Leaderboard"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"stat",    {{"type", "string"}}},
                        {"players", {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/LeaderboardPlayer"}}}}}
                    }}
                }}}}}}
            }},
            {"500", {
                {"description", "Database error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/games/recent
    paths["/api/games/recent"]["get"] = {
        {"tags", json::array({"Games"})},
        {"summary", "Recent games (paginated)"},
        {"description", "Returns a cursor-paginated list of recent NBA games, "
                        "optionally filtered by season type."},
        {"operationId", "getGamesRecent"},
        {"parameters", json::array({
            {{"$ref", "#/components/parameters/LimitParam"}},
            {{"$ref", "#/components/parameters/CursorParam"}},
            {{"name", "type"}, {"in", "query"},
             {"description", "Filter by season type"},
             {"schema", {{"type", "string"}, {"enum", json::array({"regular", "playoffs"})}}}}
        })},
        {"responses", {
            {"200", {
                {"description", "Paginated list of games"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"data",        {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/GameSummary"}}}}},
                        {"has_more",    {{"type", "boolean"}}},
                        {"next_cursor", {{"type", "string"}}}
                    }}
                }}}}}}
            }},
            {"500", {
                {"description", "Database error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/players/search
    paths["/api/players/search"]["get"] = {
        {"tags", json::array({"Search"})},
        {"summary", "Search players by name"},
        {"description", "Searches for players whose name contains the query string "
                        "(case-insensitive). Returns season stats for each match."},
        {"operationId", "searchPlayers"},
        {"parameters", json::array({
            {{"name", "q"}, {"in", "query"}, {"required", true},
             {"description", "Search query (minimum 2 characters)"},
             {"schema", {{"type", "string"}, {"minLength", 2}}}},
            {{"$ref", "#/components/parameters/LimitParam"}},
            {{"$ref", "#/components/parameters/CursorParam"}}
        })},
        {"responses", {
            {"200", {
                {"description", "Paginated search results"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"query",       {{"type", "string"}}},
                        {"data",        {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/PlayerSearchResult"}}}}},
                        {"has_more",    {{"type", "boolean"}}},
                        {"next_cursor", {{"type", "string"}}}
                    }}
                }}}}}}
            }},
            {"400", {
                {"description", "Query too short"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }},
            {"500", {
                {"description", "Database error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/games/search
    paths["/api/games/search"]["get"] = {
        {"tags", json::array({"Search"})},
        {"summary", "Search games by team"},
        {"description", "Returns games where the given team played (home or away), "
                        "sorted by date descending."},
        {"operationId", "searchGames"},
        {"parameters", json::array({
            {{"name", "team"}, {"in", "query"}, {"required", true},
             {"description", "Team tricode (e.g. BOS, LAL, GSW)"},
             {"schema", {{"type", "string"}}}},
            {{"$ref", "#/components/parameters/LimitParam"}},
            {{"$ref", "#/components/parameters/CursorParam"}}
        })},
        {"responses", {
            {"200", {
                {"description", "Paginated game results"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"team",        {{"type", "string"}}},
                        {"data",        {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/GameSearchResult"}}}}},
                        {"has_more",    {{"type", "boolean"}}},
                        {"next_cursor", {{"type", "string"}}}
                    }}
                }}}}}}
            }},
            {"400", {
                {"description", "Missing team parameter"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }},
            {"500", {
                {"description", "Database error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/events/search
    paths["/api/events/search"]["get"] = {
        {"tags", json::array({"Search"})},
        {"summary", "Search play-by-play events"},
        {"description", "Returns play events filtered by player, action type, and/or "
                        "game. At least one filter parameter is required."},
        {"operationId", "searchEvents"},
        {"parameters", json::array({
            {{"name", "player_id"}, {"in", "query"},
             {"description", "Filter by player ID"},
             {"schema", {{"type", "integer"}}}},
            {{"name", "action_type"}, {"in", "query"},
             {"description", "Filter by action type"},
             {"schema", {{"type", "string"},
                         {"enum", json::array({"2pt", "3pt", "freethrow", "rebound",
                                               "steal", "block", "turnover", "foul",
                                               "substitution", "timeout", "jumpball",
                                               "period", "violation", "ejection"})}}}},
            {{"name", "game_id"}, {"in", "query"},
             {"description", "Filter by game ID"},
             {"schema", {{"type", "string"}}}},
            {{"name", "limit"}, {"in", "query"},
             {"description", "Maximum events to return (default 50, max 200)"},
             {"schema", {{"type", "integer"}, {"default", 50}, {"minimum", 1}, {"maximum", 200}}}}
        })},
        {"responses", {
            {"200", {
                {"description", "Matching events"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"count",  {{"type", "integer"}}},
                        {"events", {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/EventSearchResult"}}}}}
                    }}
                }}}}}}
            }},
            {"400", {
                {"description", "Missing filter or invalid action_type"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }},
            {"500", {
                {"description", "Database error"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/scoreboard
    paths["/api/scoreboard"]["get"] = {
        {"tags", json::array({"Games"})},
        {"summary", "Live scoreboard"},
        {"description", "Returns the current live scoreboard snapshot from the "
                        "live ingestor. Requires --live mode."},
        {"operationId", "getScoreboard"},
        {"responses", {
            {"200", {
                {"description", "Live scoreboard"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"games", {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/ScoreboardGame"}}}}}
                    }}
                }}}}}}
            }},
            {"503", {
                {"description", "Live ingestion not enabled"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/elo
    paths["/api/elo"]["get"] = {
        {"tags", json::array({"Analytics"})},
        {"summary", "Elo rankings (paginated)"},
        {"description", "Returns all teams ranked by Elo rating, with cursor pagination."},
        {"operationId", "getEloRankings"},
        {"parameters", json::array({
            {{"$ref", "#/components/parameters/LimitParam"}},
            {{"$ref", "#/components/parameters/CursorParam"}}
        })},
        {"responses", {
            {"200", {
                {"description", "Paginated Elo rankings"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"data",             {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/EloTeam"}}}}},
                        {"has_more",         {{"type", "boolean"}}},
                        {"next_cursor",      {{"type", "string"}}},
                        {"games_processed",  {{"type", "integer"}}},
                        {"build_ms",         {{"type", "number"}}}
                    }}
                }}}}}}
            }},
            {"503", {
                {"description", "Elo ratings not ready"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/elo/history
    paths["/api/elo/history"]["get"] = {
        {"tags", json::array({"Analytics"})},
        {"summary", "Elo rating history"},
        {"description", "Returns historical Elo rating snapshots per team per season."},
        {"operationId", "getEloHistory"},
        {"responses", {
            {"200", {
                {"description", "Elo history snapshots"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"snapshots", {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/EloSnapshot"}}}}}
                    }}
                }}}}}}
            }},
            {"503", {
                {"description", "Elo ratings not ready"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    // GET /api/index/status
    paths["/api/index/status"]["get"] = {
        {"tags", json::array({"Analytics"})},
        {"summary", "Similarity index status"},
        {"description", "Returns whether the HNSW similarity index is loaded and its size."},
        {"operationId", "getIndexStatus"},
        {"responses", {
            {"200", {
                {"description", "Index status"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/IndexStatus"}}}}}}}
            }}
        }}
    };

    // GET /api/similar
    paths["/api/similar"]["get"] = {
        {"tags", json::array({"Analytics"})},
        {"summary", "Find similar game states"},
        {"description", "Queries the HNSW similarity index for historical game "
                        "states most similar to the provided parameters."},
        {"operationId", "getSimilar"},
        {"parameters", json::array({
            {{"name", "score_home"}, {"in", "query"},
             {"description", "Home team score"},
             {"schema", {{"type", "integer"}, {"default", 50}}}},
            {{"name", "score_away"}, {"in", "query"},
             {"description", "Away team score"},
             {"schema", {{"type", "integer"}, {"default", 50}}}},
            {{"name", "period"}, {"in", "query"},
             {"description", "Game period (1-4, 5+ for OT)"},
             {"schema", {{"type", "integer"}, {"default", 2}}}},
            {{"name", "clock"}, {"in", "query"},
             {"description", "Seconds remaining in period"},
             {"schema", {{"type", "integer"}, {"default", 360}}}},
            {{"name", "momentum"}, {"in", "query"},
             {"description", "Momentum indicator"},
             {"schema", {{"type", "integer"}, {"default", 0}}}},
            {{"name", "k"}, {"in", "query"},
             {"description", "Number of similar results to return (1-25)"},
             {"schema", {{"type", "integer"}, {"default", 10}, {"minimum", 1}, {"maximum", 25}}}}
        })},
        {"responses", {
            {"200", {
                {"description", "Similar game states"},
                {"content", {{"application/json", {{"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"query",      {{"type", "object"}}},
                        {"query_ms",   {{"type", "number"}}},
                        {"index_size", {{"type", "integer"}}},
                        {"results",    {{"type", "array"}, {"items", {{"$ref", "#/components/schemas/SimilarResult"}}}}}
                    }}
                }}}}}}
            }},
            {"503", {
                {"description", "Similarity index not ready"},
                {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}
            }}
        }}
    };

    return spec;
}

// ── Handlers ────────────────────────────────────────────────────────────────

void handle_openapi(Request& /*req*/, Response& res, ServerContext& /*ctx*/) {
    static const std::string spec_json = build_openapi_spec().dump();
    res.json(spec_json);
}

void handle_docs(Request& /*req*/, Response& res, ServerContext& /*ctx*/) {
    static const std::string html = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Cortex API — Documentation</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui.css">
  <style>
    html { box-sizing: border-box; overflow-y: scroll; }
    *, *::before, *::after { box-sizing: inherit; }
    body { margin: 0; background: #fafafa; }
    .topbar { display: none; }
  </style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui-bundle.js"></script>
  <script>
    SwaggerUIBundle({
      url: '/api/openapi.json',
      dom_id: '#swagger-ui',
      deepLinking: true,
      presets: [
        SwaggerUIBundle.presets.apis,
        SwaggerUIBundle.SwaggerUIStandalonePreset
      ],
      layout: 'BaseLayout'
    });
  </script>
</body>
</html>)html";

    res.send_file(html, "text/html");
}

} // namespace cortex::serving::handlers
