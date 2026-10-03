#pragma once

#include <string>
#include <vector>

#include "aim/scenario/replay.h"

namespace aim {

enum class AnalysisSeverity { INFO, WARNING, CRITICAL };

struct AnalysisFinding {
  AnalysisSeverity severity = AnalysisSeverity::INFO;
  std::string category;
  std::string title;
  std::string detail;
  float timestamp_seconds = -1.0f;
};

struct ScenarioAnalysis {
  std::string scenario_name;
  float duration_seconds = 0;
  float score = 0;
  int clicks = 0;
  int hits = 0;
  int misses = 0;
  float accuracy_percent = 0;
  float average_click_interval_ms = 0;
  int longest_miss_streak = 0;
  float average_tracking_error = 0;
  float worst_tracking_error = 0;
  float average_mouse_speed = 0;
  float peak_mouse_speed = 0;
  float high_speed_percent = 0;
  float direction_change_rate = 0;
  float target_direction_change_rate = 0;
  float average_target_speed = 0;
  float peak_target_speed = 0;
  float tracking_time_percent = 0;
  float time_above_1deg_percent = 0;
  float time_above_2deg_percent = 0;
  float time_above_5deg_percent = 0;
  float largest_error_timestamp = -1;
  float longest_loss_duration_seconds = 0;
  float average_recovery_time_ms = 0;
  int tracking_loss_count = 0;
  bool has_target_snapshots = false;
  std::vector<AnalysisFinding> findings;
  std::string timeline_summary;
  std::string tracking_summary;
  std::string deterministic_summary;
};

ScenarioAnalysis AnalyzeScenarioReplay(const Replay& replay);
std::string AnalysisSeverityLabel(AnalysisSeverity severity);
std::string BuildNimAnalysisPrompt(const ScenarioAnalysis& analysis);

}  // namespace aim
