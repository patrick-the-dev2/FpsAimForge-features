#include "scenario_analysis.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "aim/common/geometry.h"
#include "aim/common/util.h"
#include "aim/core/camera.h"

namespace aim {
namespace {

float ToDegrees(float radians) {
  return radians * 180.0f / 3.14159265358979323846f;
}

std::string FormatTime(float seconds) {
  return std::format("{:.2f}s", seconds);
}

void AddFinding(ScenarioAnalysis* result,
                AnalysisSeverity severity,
                std::string category,
                std::string title,
                std::string detail,
                float timestamp = -1) {
  if (result->findings.size() >= 12) return;
  result->findings.push_back(
      {severity, std::move(category), std::move(title), std::move(detail), timestamp});
}

}  // namespace

std::string AnalysisSeverityLabel(AnalysisSeverity severity) {
  switch (severity) {
    case AnalysisSeverity::CRITICAL:
      return "Critical";
    case AnalysisSeverity::WARNING:
      return "Warning";
    case AnalysisSeverity::INFO:
      return "Info";
  }
  return "Info";
}

ScenarioAnalysis AnalyzeScenarioReplay(const Replay& replay) {
  ScenarioAnalysis result;
  result.scenario_name = replay.scenario_name;
  result.duration_seconds = replay.GetDurationSeconds();
  result.has_target_snapshots = !replay.target_data.empty();
  if (!replay.scores.empty()) result.score = replay.scores.back();

  std::vector<float> click_intervals_ms;
  std::vector<float> mouse_speeds;
  std::vector<float> tracking_errors;

  int miss_streak = 0;
  i64 last_click_time = -1;
  int direction_change_count = 0;
  int direction_samples = 0;
  float previous_dx = 0;
  float previous_dy = 0;

  for (const ReplayEvent& event : replay.events) {
    if (event.type != ReplayEventType::MOUSE_CLICK) continue;
    ++result.clicks;
    if (event.data.is_hit) {
      ++result.hits;
      miss_streak = 0;
    } else {
      ++result.misses;
      ++miss_streak;
      result.longest_miss_streak = std::max(result.longest_miss_streak, miss_streak);
    }
    if (last_click_time >= 0) {
      click_intervals_ms.push_back(
          static_cast<float>(event.time_micros - last_click_time) / 1000.0f);
    }
    last_click_time = event.time_micros;
  }

  result.accuracy_percent =
      result.clicks > 0 ? 100.0f * result.hits / result.clicks : 0.0f;
  if (!click_intervals_ms.empty()) {
    result.average_click_interval_ms =
        std::accumulate(click_intervals_ms.begin(), click_intervals_ms.end(), 0.0f) /
        click_intervals_ms.size();
  }

  if (replay.replay_fps > 0 && replay.pitch_yaws.size() > 1) {
    for (size_t i = 1; i < replay.pitch_yaws.size(); ++i) {
      const PitchYaw& a = replay.pitch_yaws[i - 1];
      const PitchYaw& b = replay.pitch_yaws[i];
      const float dx = b.yaw - a.yaw;
      const float dy = b.pitch - a.pitch;
      const float delta = std::sqrt(dx * dx + dy * dy);
      mouse_speeds.push_back(ToDegrees(delta) * replay.replay_fps);

      if (std::abs(dx) > 0.00001f || std::abs(dy) > 0.00001f) {
        if (direction_samples > 0 && (dx * previous_dx + dy * previous_dy) < 0) {
          ++direction_change_count;
        }
        ++direction_samples;
        previous_dx = dx;
        previous_dy = dy;
      }
    }
  }

  if (!mouse_speeds.empty()) {
    result.average_mouse_speed =
        std::accumulate(mouse_speeds.begin(), mouse_speeds.end(), 0.0f) / mouse_speeds.size();
    result.peak_mouse_speed = *std::max_element(mouse_speeds.begin(), mouse_speeds.end());
    const float threshold = std::max(180.0f, result.average_mouse_speed * 2.5f);
    int high_speed_samples = 0;
    for (float speed : mouse_speeds) {
      if (speed > threshold) ++high_speed_samples;
    }
    result.high_speed_percent =
        100.0f * high_speed_samples / mouse_speeds.size();
  }
  if (direction_samples > 0) {
    result.direction_change_rate =
        100.0f * direction_change_count / direction_samples;
  }

  if (replay.replay_fps > 0 && !replay.pitch_yaws.empty() &&
      !replay.target_metadata.empty()) {
    Camera camera(CameraParams(replay.room));
    size_t metadata_index = 0;
    size_t event_index = 0;
    std::unordered_map<u16, u16> active_targets;
    std::unordered_map<u16, ReplayTargetMetadata> metadata_by_id;
    metadata_by_id.reserve(replay.target_metadata.size());
    for (const auto& metadata : replay.target_metadata) {
      metadata_by_id.emplace(metadata.target_id, metadata);
    }

    for (size_t frame = 0; frame < replay.pitch_yaws.size(); ++frame) {
      const i64 frame_micros =
          static_cast<i64>(frame) * 1000000 / replay.replay_fps;

      while (metadata_index < replay.target_metadata.size() &&
             replay.target_metadata[metadata_index].add_time_micros <= frame_micros) {
        const auto& metadata = replay.target_metadata[metadata_index++];
        active_targets[metadata.target_id] = metadata.data_channel;
      }

      while (event_index < replay.events.size() &&
             replay.events[event_index].time_micros <= frame_micros) {
        const ReplayEvent& event = replay.events[event_index++];
        if (event.type == ReplayEventType::REMOVE_TARGET) {
          active_targets.erase(event.data.target_id);
        }
      }

      camera.UpdatePitchYaw(replay.pitch_yaws[frame]);
      const LookAtInfo look_at = camera.GetLookAt();

      float nearest_error = std::numeric_limits<float>::max();
      for (const auto& [target_id, channel] : active_targets) {
        auto metadata_it = metadata_by_id.find(target_id);
        if (metadata_it == metadata_by_id.end()) continue;

        glm::vec3 position = metadata_it->second.initial_data.position;
        float radius = metadata_it->second.initial_data.radius;
        if (!replay.target_data.empty()) {
          const i64 index = static_cast<i64>(frame) * replay.num_targets + channel;
          if (index >= 0 && index < static_cast<i64>(replay.target_data.size()) &&
              replay.target_data[index].radius > 0) {
            position = replay.target_data[index].position;
            radius = replay.target_data[index].radius;
          }
        }

        auto miss_distance = GetNormalizedMissedShotDistance(
            camera.GetPosition(), look_at.front, position);
        if (miss_distance) {
          nearest_error = std::min(
              nearest_error,
              ToDegrees(std::atan(*miss_distance * std::max(radius, 0.001f))));
        }
      }
      if (nearest_error < std::numeric_limits<float>::max()) {
        tracking_errors.push_back(nearest_error);
      }
    }
  }

  if (result.misses > 0) {
    const float miss_rate = 100.0f - result.accuracy_percent;
    AddFinding(
        &result,
        miss_rate >= 30.0f ? AnalysisSeverity::CRITICAL : AnalysisSeverity::WARNING,
        "Accuracy",
        "Miss discipline",
        std::format("{} misses across {} shots ({:.1f}% miss rate). Longest miss streak: {}.",
                    result.misses, result.clicks, miss_rate, result.longest_miss_streak));

    if (result.longest_miss_streak >= 3) {
      int streak = 0;
      for (const ReplayEvent& event : replay.events) {
        if (event.type != ReplayEventType::MOUSE_CLICK) continue;
        if (!event.data.is_hit) {
          ++streak;
          if (streak == result.longest_miss_streak) {
            AddFinding(
                &result, AnalysisSeverity::WARNING, "Failure point", "Miss streak",
                std::format("The longest miss streak reaches {} shots around {}.",
                            result.longest_miss_streak,
                            FormatTime(event.time_micros / 1000000.0f)),
                event.time_micros / 1000000.0f);
            break;
          }
        } else {
          streak = 0;
        }
      }
    }
  }

  if (result.average_click_interval_ms > 0 && result.average_click_interval_ms < 110) {
    AddFinding(&result, AnalysisSeverity::WARNING, "Pacing", "Rushed clicking",
               std::format("Average click interval is {:.0f} ms. Speed is high enough that "
                           "accuracy may be getting traded away.",
                           result.average_click_interval_ms));
  } else if (result.average_click_interval_ms > 450) {
    AddFinding(&result, AnalysisSeverity::WARNING, "Pacing", "Slow shot cycle",
               std::format("Average click interval is {:.0f} ms. There is substantial time "
                           "between shots.",
                           result.average_click_interval_ms));
  }

  if (result.high_speed_percent > 8.0f) {
    AddFinding(&result, AnalysisSeverity::WARNING, "Mouse control", "High-speed corrections",
               std::format("{:.1f}% of movement samples are above the high-speed threshold; "
                           "peak movement is {:.0f} deg/s. Review the largest flicks for "
                           "overshoot and braking.",
                           result.high_speed_percent, result.peak_mouse_speed));
  }

  if (result.direction_change_rate > 18.0f) {
    AddFinding(&result, AnalysisSeverity::WARNING, "Mouse control", "Frequent reversals",
               std::format("{:.1f}% direction-change rate suggests repeated correction cycles.",
                           result.direction_change_rate));
  }

  if (result.has_target_snapshots && result.average_tracking_error > 0) {
    AddFinding(&result,
               result.average_tracking_error > 2.5f ? AnalysisSeverity::WARNING
                                                    : AnalysisSeverity::INFO,
               "Tracking", "Crosshair-to-target distance",
               std::format("Average sampled error is {:.2f} degrees; worst sampled error is "
                           "{:.2f} degrees.",
                           result.average_tracking_error, result.worst_tracking_error));
  }

  std::ostringstream timeline;
  int shown = 0;
  for (const ReplayEvent& event : replay.events) {
    if (event.type != ReplayEventType::MOUSE_CLICK || shown >= 12) continue;
    timeline << FormatTime(event.time_micros / 1000000.0f) << " "
             << (event.data.is_hit ? "HIT" : "MISS") << "; ";
    ++shown;
  }
  result.timeline_summary = timeline.str();

  result.deterministic_summary = std::format(
      "Scenario '{}': {:.1f}s, score {:.2f}, {} hits / {} misses / {} clicks, {:.1f}% accuracy. "
      "Average click interval {:.0f} ms. Average mouse speed {:.1f} deg/s, peak {:.1f} deg/s. "
      "High-speed movement {:.1f}%. Direction-change rate {:.1f}%.",
      result.scenario_name, result.duration_seconds, result.score, result.hits, result.misses,
      result.clicks, result.accuracy_percent, result.average_click_interval_ms,
      result.average_mouse_speed, result.peak_mouse_speed, result.high_speed_percent,
      result.direction_change_rate);

  return result;
}

std::string BuildNimAnalysisPrompt(const ScenarioAnalysis& analysis) {
  std::ostringstream prompt;
  prompt << "You are an FPS aim coach reviewing one completed aim-trainer scenario. "
            "Use only the supplied replay-derived facts. Do not invent events or claim visual "
            "information that is not present. Give a practical diagnostic.\\n\\n";
  prompt << "Scenario details:\\n" << analysis.deterministic_summary << "\\n";
  prompt << "Target snapshots: " << (analysis.has_target_snapshots ? "available" : "not available")
         << "\\n";
  prompt << "Replay timeline sample: " << analysis.timeline_summary << "\\n\\n";
  prompt << "Detected findings:\\n";
  for (const auto& finding : analysis.findings) {
    prompt << "- [" << AnalysisSeverityLabel(finding.severity) << "] "
           << finding.category << ": " << finding.title << " - " << finding.detail;
    if (finding.timestamp_seconds >= 0) {
      prompt << " (around " << FormatTime(finding.timestamp_seconds) << ")";
    }
    prompt << "\\n";
  }
  prompt << "\\nReturn these headings exactly:\\n"
            "AI Overview\\n"
            "What Happened\\n"
            "Weak Points\\n"
            "Where It Failed\\n"
            "What To Practice Next\\n"
            "Replay Review\\n"
            "Keep advice specific to the measured run and state uncertainty where appropriate.";
  return prompt.str();
}

}  // namespace aim
