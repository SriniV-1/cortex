#include "etl/NBAClient.hpp"
#include "common/Logger.hpp"

#include <curl/curl.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstring>
#include <format>

namespace cortex::etl {

static constexpr const char* NBA_S3_BASE =
    "https://nba-prod-us-east-1-mediaops-stats.s3.amazonaws.com/NBA/liveData";

static constexpr const char* USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

// ── libcurl write callback ─────────────────────────────────────────────────
static size_t write_cb(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ── RAII CURL handle ───────────────────────────────────────────────────────
struct NBAClient::Impl {
    CURL* curl = nullptr;
    std::shared_ptr<spdlog::logger> log;

    Impl() : log(cortex::get_logger("nba_client")) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");
    }
    ~Impl() {
        if (curl) curl_easy_cleanup(curl);
        curl_global_cleanup();
    }
};

NBAClient::NBAClient(int request_delay_ms)
    : impl_(std::make_unique<Impl>()),
      request_delay_ms_(request_delay_ms) {}

NBAClient::~NBAClient() = default;

// ── HTTP GET ───────────────────────────────────────────────────────────────
std::string NBAClient::http_get(const std::string& url) const {
    std::string response;
    CURL* curl = impl_->curl;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        throw std::runtime_error(
            std::string("curl error: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code == 403 || http_code == 404) {
        // Game simply doesn't exist in this S3 feed — not an error worth logging.
        throw HttpNotFoundError(std::format("HTTP {} for {}", http_code, url));
    }
    if (http_code != 200) {
        throw std::runtime_error(
            std::format("HTTP {} for {}", http_code, url));
    }

    // Rate-limit: sleep between requests
    if (request_delay_ms_ > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(request_delay_ms_));
    }

    return response;
}

// ── Scoreboard ────────────────────────────────────────────────────────────
std::vector<GameSummary> NBAClient::fetch_scoreboard() const {
    const std::string url = std::string(NBA_S3_BASE)
                          + "/scoreboard/todaysScoreboard_00.json";

    impl_->log->info("Fetching scoreboard: {}", url);
    const std::string body = http_get(url);

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(body);
    simdjson::ondemand::document doc = parser.iterate(padded);

    std::vector<GameSummary> results;

    for (auto game : doc["scoreboard"]["games"]) {
        GameSummary gs;
        gs.game_id       = std::string(std::string_view(game["gameId"]));
        gs.status        = static_cast<int>(int64_t(game["gameStatus"]));
        gs.away_tricode  = std::string(std::string_view(game["awayTeam"]["teamTricode"]));
        gs.home_tricode  = std::string(std::string_view(game["homeTeam"]["teamTricode"]));
        gs.away_team_id  = static_cast<int>(int64_t(game["awayTeam"]["teamId"]));
        gs.home_team_id  = static_cast<int>(int64_t(game["homeTeam"]["teamId"]));

        // Scores may be absent for scheduled games
        auto away_score_val = game["awayTeam"]["score"];
        auto home_score_val = game["homeTeam"]["score"];
        gs.away_score = (away_score_val.error() == simdjson::SUCCESS)
                      ? static_cast<int>(int64_t(away_score_val)) : 0;
        gs.home_score = (home_score_val.error() == simdjson::SUCCESS)
                      ? static_cast<int>(int64_t(home_score_val)) : 0;

        results.push_back(std::move(gs));
    }

    impl_->log->info("Scoreboard: {} games", results.size());
    return results;
}

// ── Play-by-play ──────────────────────────────────────────────────────────
std::optional<PlayByPlay> NBAClient::fetch_play_by_play(
        const std::string& game_id) const {

    const std::string url = std::string(NBA_S3_BASE)
                          + "/playbyplay/playbyplay_" + game_id + ".json";

    std::string body;
    try {
        body = http_get(url);
    } catch (const HttpNotFoundError&) {
        return std::nullopt;   // 403/404 — game not in S3 feed, expected, no log
    } catch (const std::exception& e) {
        impl_->log->warn("fetch_play_by_play {} network error: {}", game_id, e.what());
        return std::nullopt;
    }

    PlayByPlay pbp;
    pbp.game_id = game_id;

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        simdjson::ondemand::document doc = parser.iterate(padded);

        for (auto action : doc["game"]["actions"]) {
            PlayAction pa{};
            pa.action_number = int64_t(action["actionNumber"]);
            pa.period        = static_cast<int>(int64_t(action["period"]));
            pa.order_number  = int64_t(action["orderNumber"]);

            // String fields — use .value_unsafe() for speed (JSON is trusted source)
            auto get_str = [](auto&& val) -> std::string {
                auto sv = val.get_string();
                return sv.error() == simdjson::SUCCESS
                     ? std::string(sv.value()) : "";
            };

            pa.clock        = get_str(action["clock"]);
            pa.time_actual  = get_str(action["timeActual"]);
            pa.period_type  = get_str(action["periodType"]);
            pa.action_type  = get_str(action["actionType"]);
            pa.sub_type     = get_str(action["subType"]);
            pa.description  = get_str(action["description"]);

            auto pid = action["personId"].get_int64();
            pa.person_id = (pid.error() == simdjson::SUCCESS)
                         ? static_cast<int32_t>(pid.value()) : 0;

            // x/y may be null
            auto xv = action["x"].get_double();
            auto yv = action["y"].get_double();
            pa.x = (xv.error() == simdjson::SUCCESS) ? static_cast<float>(xv.value()) : 0.0f;
            pa.y = (yv.error() == simdjson::SUCCESS) ? static_cast<float>(yv.value()) : 0.0f;

            auto sh = action["scoreHome"].get_string();
            auto sa = action["scoreAway"].get_string();
            pa.score_home = (sh.error() == simdjson::SUCCESS && !sh.value().empty())
                          ? static_cast<int16_t>(std::stoi(std::string(sh.value()))) : 0;
            pa.score_away = (sa.error() == simdjson::SUCCESS && !sa.value().empty())
                          ? static_cast<int16_t>(std::stoi(std::string(sa.value()))) : 0;

            // Store qualifiers as raw JSON for JSONB storage
            pa.qualifiers_json = "[]"; // default

            pbp.actions.push_back(std::move(pa));
        }
    } catch (const simdjson::simdjson_error& e) {
        impl_->log->warn("fetch_play_by_play {}: malformed JSON, skipping — {}",
                         game_id, e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        impl_->log->warn("fetch_play_by_play {}: parse error, skipping — {}",
                         game_id, e.what());
        return std::nullopt;
    }

    impl_->log->debug("Fetched {} actions for game {}", pbp.actions.size(), game_id);
    return pbp;
}

// ── Boxscore ──────────────────────────────────────────────────────────────
std::optional<BoxScore> NBAClient::fetch_boxscore(const std::string& game_id) const {
    const std::string url = std::string(NBA_S3_BASE)
                          + "/boxscore/boxscore_" + game_id + ".json";

    std::string body;
    try {
        body = http_get(url);
    } catch (const HttpNotFoundError&) {
        return std::nullopt;   // 403/404 — game not in S3 feed, expected, no log
    } catch (const std::exception& e) {
        impl_->log->warn("fetch_boxscore {} network error: {}", game_id, e.what());
        return std::nullopt;
    }

    BoxScore bs;
    bs.game.game_id = game_id;

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        simdjson::ondemand::document doc = parser.iterate(padded);

        auto get_str = [](auto&& val) -> std::string {
            auto sv = val.get_string();
            return sv.error() == simdjson::SUCCESS ? std::string(sv.value()) : "";
        };

        auto game_obj = doc["game"];

        // Game date: parse first 10 chars of gameTimeUTC ("2024-12-13T00:30:00Z")
        std::string game_time_utc = get_str(game_obj["gameTimeUTC"]);
        bs.game.game_date = (game_time_utc.size() >= 10)
                          ? game_time_utc.substr(0, 10)
                          : "";

        auto status_val = game_obj["gameStatus"].get_int64();
        bs.game.status = (status_val.error() == simdjson::SUCCESS)
                       ? static_cast<int>(status_val.value()) : 3;

        // Parse one team (home or away) into TeamInfo + GameSummary fields
        auto parse_team = [&](auto&& team_obj, bool is_home) {
            TeamInfo ti;
            ti.team_id = static_cast<int32_t>(int64_t(team_obj["teamId"]));
            ti.tricode  = get_str(team_obj["teamTricode"]);
            ti.name     = get_str(team_obj["teamName"]);
            ti.city     = get_str(team_obj["teamCity"]);

            auto score_val = team_obj["score"].get_int64();
            int score = (score_val.error() == simdjson::SUCCESS)
                      ? static_cast<int>(score_val.value()) : 0;

            if (is_home) {
                bs.game.home_team_id = ti.team_id;
                bs.game.home_tricode = ti.tricode;
                bs.game.home_score   = score;
                bs.home_team         = ti;
            } else {
                bs.game.away_team_id = ti.team_id;
                bs.game.away_tricode = ti.tricode;
                bs.game.away_score   = score;
                bs.away_team         = ti;
            }

            // Parse players
            std::vector<PlayerInfo> roster;
            for (auto player : team_obj["players"]) {
                PlayerInfo pi{};
                auto pid = player["personId"].get_int64();
                if (pid.error() != simdjson::SUCCESS) continue;
                pi.person_id  = static_cast<int32_t>(pid.value());
                pi.first_name = get_str(player["firstName"]);
                pi.last_name  = get_str(player["familyName"]);
                pi.team_id    = ti.team_id;
                pi.position   = get_str(player["position"]);

                // jerseyNum is a string like "7" or "" — convert or use -1
                std::string jnum = get_str(player["jerseyNum"]);
                pi.jersey_num = (!jnum.empty() && std::isdigit(jnum[0]))
                              ? static_cast<int16_t>(std::stoi(jnum))
                              : int16_t{-1};

                roster.push_back(std::move(pi));
            }
            return roster;
        };

        bs.home_players = parse_team(game_obj["homeTeam"], true);
        bs.away_players = parse_team(game_obj["awayTeam"], false);

    } catch (const simdjson::simdjson_error& e) {
        impl_->log->warn("fetch_boxscore {}: malformed JSON, skipping — {}",
                         game_id, e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        impl_->log->warn("fetch_boxscore {}: parse error, skipping — {}",
                         game_id, e.what());
        return std::nullopt;
    }

    impl_->log->debug("Boxscore {}: {} vs {} — {} home players, {} away players",
                      game_id, bs.game.home_tricode, bs.game.away_tricode,
                      bs.home_players.size(), bs.away_players.size());
    return bs;
}

// ── Game ID generation ────────────────────────────────────────────────────
// NBA game ID format: PPSSYYNNNN
//   PP = 00 (NBA)
//   SS = season type (2=regular, 4=playoffs)
//   YY = last 2 digits of season start year
//   NNNN = 0001-based counter
std::vector<std::string> NBAClient::game_ids_for_season(int season, int count,
                                                          int season_type) {
    std::vector<std::string> ids;
    ids.reserve(count);
    int yy = season % 100;
    int st = (season_type == 4) ? 4 : 2;
    for (int i = 1; i <= count; ++i) {
        ids.push_back(std::format("00{}{:02d}{:05d}", st, yy, i));
    }
    return ids;
}

} // namespace cortex::etl
