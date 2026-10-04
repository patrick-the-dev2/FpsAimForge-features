#include "aim/analysis/scenario_analysis.h"
#include "aim/database/aim_db.h"

#include "gtest/gtest.h"

namespace aim {
namespace {

ReplayEvent MakeClick(i64 time_micros, bool is_hit) {
  ReplayEvent event;
  event.type = ReplayEventType::MOUSE_CLICK;
  event.time_micros = time_micros;
  event.data.is_hit = is_hit;
  return event;
}

TEST(ScenarioAnalysisTest, ReportsClickAccuracyAndMissStreak) {
  Replay replay;
  replay.scenario_name = "Test Scenario";
  replay.replay_fps = 100;
  replay.pitch_yaws.resize(100);

  replay.events.push_back(MakeClick(100000, true));
  replay.events.push_back(MakeClick(300000, false));
  replay.events.push_back(MakeClick(500000, false));
  replay.events.push_back(MakeClick(700000, false));

  ScenarioAnalysis analysis = AnalyzeScenarioReplay(replay);

  EXPECT_EQ(analysis.clicks, 4);
  EXPECT_EQ(analysis.hits, 1);
  EXPECT_EQ(analysis.misses, 3);
  EXPECT_EQ(analysis.longest_miss_streak, 3);
  EXPECT_FLOAT_EQ(analysis.accuracy_percent, 25.0f);
  EXPECT_NEAR(analysis.average_click_interval_ms, 200.0f, 0.01f);
  EXPECT_FALSE(analysis.findings.empty());
}


TEST(ScenarioAnalysisTest, ReportsContinuousTrackingLosses) {
  Replay replay;
  replay.scenario_name = "Tracking Test";
  replay.replay_fps = 10;
  replay.num_targets = 1;
  replay.room.mutable_camera_position()->set_x(0);
  replay.room.mutable_camera_position()->set_y(0);
  replay.room.mutable_camera_position()->set_z(0);
  replay.room.set_start_pitch(0);
  replay.room.set_start_yaw(0);
  replay.pitch_yaws.resize(10);
  replay.target_data.resize(10);

  ReplayTargetMetadata metadata;
  metadata.add_time_micros = 0;
  metadata.target_id = 1;
  metadata.data_channel = 0;
  metadata.initial_data.position = {0, 10, 0};
  metadata.initial_data.radius = 1;
  replay.target_metadata.push_back(metadata);

  for (int i = 0; i < 10; ++i) {
    replay.pitch_yaws[i].pitch = 0;
    replay.pitch_yaws[i].yaw = i < 2 ? 0.0f : 0.15f;
    replay.target_data[i].position = {0, 10, 0};
    replay.target_data[i].radius = 1;
  }

  ScenarioAnalysis analysis = AnalyzeScenarioReplay(replay);

  EXPECT_GT(analysis.tracking_loss_count, 0);
  EXPECT_GT(analysis.time_above_5deg_percent, 0.0f);
  EXPECT_GT(analysis.longest_loss_duration_seconds, 0.0f);
  EXPECT_GT(analysis.largest_error_timestamp, 0.0f);
  EXPECT_NE(analysis.tracking_summary.find("Tracking losses"), std::string::npos);
}

TEST(ScenarioAnalysisTest, IncludesPersistedRunHistory) {
  ScenarioAnalysis analysis;
  analysis.deterministic_summary = "current measured run";
  analysis.cm_per_360 = 15.625f;
  analysis.tracking_summary = "measured tracking";
  analysis.timeline_summary = "0.25s HIT";

  StatsDbRow history_row;
  history_row.stats_id = 42;
  history_row.epoch_seconds = 0;
  history_row.score = 275.125;
  history_row.cm_per_360 = 31.375;
  history_row.info.set_num_hits(3);
  history_row.info.set_num_shots(4);

  const std::vector<StatsDbRow> history{history_row};
  const std::string prompt = BuildNimAnalysisPrompt(analysis, history);

  EXPECT_NE(prompt.find("run=42"), std::string::npos);
  EXPECT_NE(prompt.find("1970-01-01 00:00:00 UTC"), std::string::npos);
  EXPECT_NE(prompt.find("score=275.125"), std::string::npos);
  EXPECT_NE(prompt.find("cm/360=31.375"), std::string::npos);
  EXPECT_NE(prompt.find("accuracy=75.00%"), std::string::npos);
  EXPECT_NE(prompt.find("Exact cm/360: 15.625"), std::string::npos);
}

TEST(ScenarioAnalysisTest, BuildsCoachPromptFromMeasuredData) {
  ScenarioAnalysis analysis;
  analysis.scenario_name = "Prompt Test";
  analysis.deterministic_summary = "measured summary";
  analysis.tracking_summary = "tracking losses: 2; average recovery 310 ms";
  analysis.timeline_summary = "0.20s MISS";
  analysis.findings.push_back(
      {AnalysisSeverity::WARNING, "Accuracy", "Miss discipline", "Measured miss detail", 0.2f});

  const std::string prompt = BuildNimAnalysisPrompt(analysis);

  EXPECT_NE(prompt.find("AI Overview"), std::string::npos);
  EXPECT_NE(prompt.find("Weak Points"), std::string::npos);
  EXPECT_NE(prompt.find("Measured miss detail"), std::string::npos);
  EXPECT_NE(prompt.find("tracking losses: 2"), std::string::npos);
}

}  // namespace
}  // namespace aim
