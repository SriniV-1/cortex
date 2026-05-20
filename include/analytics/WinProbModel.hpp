#pragma once
// WinProbModel — wraps an ONNXRuntime session for win probability inference.
//
// Model spec (win_prob.onnx):
//   Input:  "X"        float32 [1, 7]
//             [score_diff, quarter, sec_remaining, home_advantage, momentum,
//              elo_diff, elo_expected]
//   Output: "win_prob"  float32 [1, 1]
//             probability that the home team wins (0..1)
//
// Thread safety: single session, inference is thread-safe in ORT.
// Cold-start: session is loaded once in the constructor.

#include <onnxruntime_cxx_api.h>
#include <string>
#include <array>
#include <stdexcept>

namespace cortex::analytics {

struct WinProbInput {
    float score_diff;       // home_score - away_score
    float quarter;          // 1..4 (or 5+ for OT)
    float sec_remaining;    // seconds remaining in game
    float home_advantage;   // 1.0 if home team, 0.0 if away
    float momentum;         // rolling point differential last 10 events (normalized)
    float elo_diff;         // home_elo - away_elo
    float elo_expected;     // Elo expected win probability for home team
};

class WinProbModel {
public:
    explicit WinProbModel(const std::string& model_path);

    // Run inference. Returns home win probability in [0, 1].
    float predict(const WinProbInput& input) const;

    bool loaded() const noexcept { return session_ != nullptr; }

private:
    Ort::Env                       env_;
    std::unique_ptr<Ort::Session>  session_;
    Ort::AllocatorWithDefaultOptions allocator_;

    static constexpr const char* INPUT_NAME  = "X";
    static constexpr const char* OUTPUT_NAME = "win_prob";
};

} // namespace cortex::analytics
