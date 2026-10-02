#pragma once

#include <memory>
#include <mutex>
#include <string>

namespace aim {

struct NimAnalysisState {
  mutable std::mutex mutex;
  bool done = false;
  bool success = false;
  std::string response;
  std::string error;
};

std::shared_ptr<NimAnalysisState> StartNimAnalysis(const std::string& prompt);
bool IsNimConfigured();
std::string GetNimConfigurationHint();

}  // namespace aim
