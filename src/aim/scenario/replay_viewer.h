#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aim/analysis/nim_client.h"
// ReplayExportState is defined in nim_client.h above.

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

  // If set, skip NIM and encode frames to this path as an MP4 instead.
  std::filesystem::path export_output_path;
  std::shared_ptr<ReplayExportState> export_state;
  int export_capture_fps = 30;
};

// Capture state for plain MP4 export (no NIM involved).
struct ReplayExportCaptureState {
  std::filesystem::path output_path;
  // Set by CreateReplayExportViewerScreen after construction.
  std::shared_ptr<VisualReplayCaptureState> vis_capture;
};
// Not forward declaring this messes up the build in windows for some reason.
struct Replay;

std::unique_ptr<Screen> CreateReplayViewerScreen(
    std::shared_ptr<Replay> replay,
    Application* app,
    std::shared_ptr<VisualReplayCaptureState> capture_state = nullptr);

// Launches the replay viewer purely to capture frames for MP4 export.
std::unique_ptr<Screen> CreateReplayExportViewerScreen(
    std::shared_ptr<Replay> replay,
    Application* app,
    std::shared_ptr<ReplayExportCaptureState> export_capture_state);

}  // namespace aim
