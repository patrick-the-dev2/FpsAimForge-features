#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aim/analysis/nim_client.h"

#include "aim/core/screen.h"

namespace aim {

class Application;

struct VisualReplayCaptureState {
  mutable std::mutex mutex;
  bool active = true;
  bool done = false;
  float progress = 0;
  std::string error;
  std::string prompt;
  std::vector<std::filesystem::path> image_paths;
  std::shared_ptr<NimAnalysisState> nim_state;
};
// Not forward declaring this messes up the build in windows for some reason.
struct Replay;

std::unique_ptr<Screen> CreateReplayViewerScreen(
    std::shared_ptr<Replay> replay,
    Application* app,
    std::shared_ptr<VisualReplayCaptureState> capture_state = nullptr);

}  // namespace aim
