// CORTEX ETL — Historical NBA data loader
// Usage: cortex_etl --season 2023 [--threads 8] [--dry-run]
//        cortex_etl --populate-dimensions [--threads 8]   ← backfill games/teams/players

#include "etl/NBAClient.hpp"
#include "etl/BulkInserter.hpp"
#include "common/Logger.hpp"

#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>

static constexpr const char* DEFAULT_CONN =
    "host=localhost port=5433 dbname=cortex";

static void print_usage() {
    std::cerr
        << "Usage: cortex_etl [options]\n"
        << "  --season   YEAR     Season start year to load (default: 2023)\n"
        << "  --threads  N        Parallel fetch threads (default: 4)\n"
        << "  --conn     STR      libpqxx connection string\n"
        << "  --dry-run           Fetch JSON but do not write to DB\n"
        << "  --populate-dimensions\n"
        << "                      Backfill games/teams/players for all already-loaded\n"
        << "                      games that are missing from the dimension tables.\n"
        << "                      Does not re-insert play events.\n"
        << "  --help              Show this help\n";
}

// ── Normal load: fetch play-by-play + boxscore for one season ─────────────
static void run_season_load(
        const std::string& conn_str,
        int season,
        int num_threads,
        bool dry_run,
        std::shared_ptr<spdlog::logger> log)
{
    auto game_ids = cortex::etl::NBAClient::game_ids_for_season(season, 1230);
    log->info("Generated {} candidate game IDs for season {}", game_ids.size(), season);

    std::mutex queue_mu;
    size_t next_idx = 0;
    std::atomic<int64_t> total_events{0};
    std::atomic<int>     total_games{0};

    auto worker = [&](int thread_id) {
        cortex::etl::NBAClient   client(150);
        cortex::etl::BulkInserter inserter(conn_str);

        while (true) {
            std::string game_id;
            {
                std::lock_guard lock(queue_mu);
                if (next_idx >= game_ids.size()) break;
                game_id = game_ids[next_idx++];
            }

            // Skip events already loaded, but still attempt dimension upsert
            // in case the previous run predated dimension support.
            bool events_already_loaded = !dry_run && inserter.is_game_loaded(game_id);

            // ── Step 1: boxscore → dimension tables ──────────────────────
            auto bs = client.fetch_boxscore(game_id);
            if (bs && !dry_run) {
                inserter.ensure_team(bs->home_team);
                inserter.ensure_team(bs->away_team);
                inserter.upsert_game(bs->game);

                auto all_players = bs->home_players;
                all_players.insert(all_players.end(),
                                   bs->away_players.begin(),
                                   bs->away_players.end());
                inserter.upsert_players(all_players);
            }

            if (events_already_loaded) continue;

            // ── Step 2: play-by-play → play_events ──────────────────────
            auto pbp = client.fetch_play_by_play(game_id);
            if (!pbp) continue;

            if (dry_run) {
                log->info("[dry-run] Would insert {} actions for {}",
                          pbp->actions.size(), game_id);
                continue;
            }

            int64_t n = inserter.bulk_insert_play_by_play(*pbp, season);
            total_events += n;
            if (n > 0) ++total_games;
        }

        log->info("Thread {} done. Games={} Events={}",
                  thread_id,
                  inserter.stats().games_inserted,
                  inserter.stats().events_inserted);
    };

    auto start = std::chrono::steady_clock::now();

    std::vector<std::jthread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i)
        threads.emplace_back(worker, i);
    threads.clear();  // jthread joins on destruction

    auto elapsed = std::chrono::steady_clock::now() - start;
    double secs  = std::chrono::duration<double>(elapsed).count();
    log->info("Season load complete in {:.1f}s — {} games, {} events ({:.0f} ev/sec)",
              secs, total_games.load(), total_events.load(),
              total_events.load() / std::max(secs, 1.0));
}

// ── Dimension backfill: boxscore-only for games already in play_events ────
static void run_populate_dimensions(
        const std::string& conn_str,
        int num_threads,
        std::shared_ptr<spdlog::logger> log)
{
    // Find game_ids that have play events but are missing from the games table.
    std::vector<std::string> unsynced;
    {
        pqxx::connection conn(conn_str);
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT ep.game_id "
            "FROM   etl_progress ep "
            "LEFT JOIN games g ON ep.game_id = g.game_id "
            "WHERE  ep.status = 'done' "
            "AND    g.game_id IS NULL "
            "ORDER BY ep.season, ep.game_id"
        );
        txn.commit();
        for (auto row : r)
            unsynced.push_back(row[0].as<std::string>());
    }

    log->info("Dimension backfill: {} games need teams/games/players populated",
              unsynced.size());
    if (unsynced.empty()) {
        log->info("Nothing to backfill.");
        return;
    }

    std::mutex mu;
    size_t idx = 0;
    std::atomic<int> done{0};
    std::atomic<int> failed{0};

    auto worker = [&](int thread_id) {
        cortex::etl::NBAClient    client(150);
        cortex::etl::BulkInserter inserter(conn_str);

        while (true) {
            std::string game_id;
            {
                std::lock_guard lock(mu);
                if (idx >= unsynced.size()) break;
                game_id = unsynced[idx++];
            }

            auto bs = client.fetch_boxscore(game_id);
            if (!bs) {
                log->warn("No boxscore for {} — skipping", game_id);
                ++failed;
                continue;
            }

            inserter.ensure_team(bs->home_team);
            inserter.ensure_team(bs->away_team);
            inserter.upsert_game(bs->game);

            auto all_players = bs->home_players;
            all_players.insert(all_players.end(),
                               bs->away_players.begin(),
                               bs->away_players.end());
            inserter.upsert_players(all_players);

            int n = ++done;
            if (n % 100 == 0)
                log->info("Backfill progress: {}/{}", n, unsynced.size());
        }

        log->info("Thread {} finished backfill.", thread_id);
    };

    auto start = std::chrono::steady_clock::now();

    std::vector<std::jthread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i)
        threads.emplace_back(worker, i);
    threads.clear();

    auto elapsed = std::chrono::steady_clock::now() - start;
    double secs  = std::chrono::duration<double>(elapsed).count();
    log->info("Backfill complete in {:.1f}s — {}/{} games populated, {} failed",
              secs, done.load(), unsynced.size(), failed.load());
}

// ── main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    auto log = cortex::get_logger("etl_main");
    spdlog::set_level(spdlog::level::info);

    int season      = 2023;
    int num_threads = 4;
    bool dry_run    = false;
    bool pop_dims   = false;
    std::string conn_str = DEFAULT_CONN;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--help")                 { print_usage(); return 0; }
        else if (arg == "--dry-run")              dry_run  = true;
        else if (arg == "--populate-dimensions")  pop_dims = true;
        else if (arg == "--season"  && i+1 < argc) season      = std::stoi(argv[++i]);
        else if (arg == "--threads" && i+1 < argc) num_threads = std::stoi(argv[++i]);
        else if (arg == "--conn"    && i+1 < argc) conn_str    = argv[++i];
        else { std::cerr << "Unknown arg: " << arg << "\n"; print_usage(); return 1; }
    }

    if (pop_dims) {
        log->info("CORTEX ETL — populate-dimensions mode — {} threads", num_threads);
        run_populate_dimensions(conn_str, num_threads, log);
    } else {
        log->info("CORTEX ETL — season {} — {} threads — dry_run={}",
                  season, num_threads, dry_run);
        run_season_load(conn_str, season, num_threads, dry_run, log);
    }

    return 0;
}
