#include "analytics/WinProbModel.hpp"
#include "common/Logger.hpp"

#include <array>
#include <stdexcept>
#include <format>

namespace cortex::analytics {

WinProbModel::WinProbModel(const std::string& model_path)
    : env_(ORT_LOGGING_LEVEL_WARNING, "cortex_ort")
{
    auto log = cortex::get_logger("win_prob");

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    try {
        session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), opts);
        log->info("WinProbModel loaded: {}", model_path);
    } catch (const Ort::Exception& e) {
        log->error("Failed to load ONNX model '{}': {}", model_path, e.what());
        throw std::runtime_error(std::format("WinProbModel: {}", e.what()));
    }
}

float WinProbModel::predict(const WinProbInput& input) const {
    // Input tensor: [1, 7]
    std::array<float, 7> x_data = {
        input.score_diff,
        input.quarter,
        input.sec_remaining,
        input.home_advantage,
        input.momentum,
        input.elo_diff,
        input.elo_expected,
    };

    constexpr std::array<int64_t, 2> input_shape  = {1, 7};
    constexpr std::array<int64_t, 2> output_shape = {1, 1};

    auto memory_info = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        x_data.data(), x_data.size(),
        input_shape.data(), input_shape.size());

    const char* input_names[]  = {INPUT_NAME};
    const char* output_names[] = {OUTPUT_NAME};

    auto outputs = session_->Run(
        Ort::RunOptions{nullptr},
        input_names,  &input_tensor, 1,
        output_names, 1);

    return outputs[0].GetTensorMutableData<float>()[0];
}

} // namespace cortex::analytics
