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

}  // namespace aim