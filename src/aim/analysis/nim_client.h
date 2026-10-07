#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aim {

struct NimAnalysisState {
  mutable std::mutex mutex;
  bool done = false;
  bool success = false;
  std::string response;
  std::string error;
};

std::shared_ptr<NimAnalysisState> StartNimAnalysis(const std::string& prompt);
std::shared_ptr<NimAnalysisState> StartNimChat(const std::string& system_prompt,
                                                const std::string& conversation_prompt);
std::shared_ptr<NimAnalysisState> StartNimVisualAnalysis(
    const std::string& prompt, const std::vector<std::filesystem::path>& image_paths);
std::shared_ptr<NimAnalysisState> StartNimVideoAnalysis(
    const std::string& prompt, const std::filesystem::path& video_path);
std::shared_ptr<NimAnalysisState> StartNimVideoAnalysisFromFrames(
    const std::string& prompt, const std::vector<std::filesystem::path>& image_paths);
bool IsNimConfigured();
std::string GetNimConfigurationHint();

// Export replay frames to an MP4 file at output_path.
// Cleans up the frame PNGs after encoding regardless of success.
// On success, state->success = true and state->response = output_path.string().
// On failure, state->success = false and state->error describes what went wrong.
struct ReplayExportState {
  mutable std::mutex mutex;
  bool done = false;
  bool success = false;
  std::filesystem::path output_path;
  std::string error;
};
std::shared_ptr<ReplayExportState> StartReplayExportToMp4(
    const std::vector<std::filesystem::path>& image_paths,
    const std::filesystem::path& output_path,
    int capture_fps);

}  // namespace aim