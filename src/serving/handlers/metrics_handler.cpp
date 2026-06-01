#include "serving/handlers/metrics_handler.hpp"
#include "analytics/GameStateIndex.hpp"
#include "analytics/EloTracker.hpp"

#include <sstream>

namespace cortex::serving::handlers {

void handle_metrics(Request& /*req*/, Response& res, ServerContext& ctx) {
    std::ostringstream m;
    m << "# HELP cortex_events_processed Total stream events processed\n"
      << "# TYPE cortex_events_processed counter\n";
    if (ctx.accumulator)
        m << "cortex_events_processed " << ctx.accumulator->event_count() << "\n";

    m << "# HELP cortex_active_games Number of games currently tracked in accumulator\n"
      << "# TYPE cortex_active_games gauge\n";
    if (ctx.accumulator)
        m << "cortex_active_games " << ctx.accumulator->game_count() << "\n";

    m << "# HELP cortex_rate_limiter_buckets Number of active rate limiter buckets\n"
      << "# TYPE cortex_rate_limiter_buckets gauge\n";
    if (ctx.rate_limiter)
        m << "cortex_rate_limiter_buckets " << ctx.rate_limiter->bucket_count() << "\n";

    m << "# HELP cortex_similarity_index_size Number of vectors in similarity index\n"
      << "# TYPE cortex_similarity_index_size gauge\n";
    if (ctx.game_state_index && ctx.game_state_index->loaded())
        m << "cortex_similarity_index_size " << ctx.game_state_index->size() << "\n";
    else
        m << "cortex_similarity_index_size 0\n";

    m << "# HELP cortex_elo_games_processed Total games used for Elo computation\n"
      << "# TYPE cortex_elo_games_processed gauge\n";
    if (ctx.elo_tracker && ctx.elo_tracker->built())
        m << "cortex_elo_games_processed " << ctx.elo_tracker->games_processed() << "\n";
    else
        m << "cortex_elo_games_processed 0\n";

    res.text(m.str());
}

} // namespace cortex::serving::handlers
