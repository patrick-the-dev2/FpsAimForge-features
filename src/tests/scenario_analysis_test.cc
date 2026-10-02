#include "aim/analysis/scenario_analysis.h"

#include "gtest/gtest.h"

namespace aim {
namespace {

TEST(ScenarioAnalysisTest, ReportsClickAccuracyAndMissStreak) {
  Replay replay;
  replay.scenario_name = "Test Scenario";
  replay.replay_fps = 100;
  replay.pitch_yaws.resize(100);

  replay.events.push_back(
      {.type = ReplayEventType::MOUSE_CLICK, .time_micros = 100000, .data = {.is_hit = true}});
  replay.events.push_back(
      {.type = ReplayEventType::MOUSE_CLICK, .time_micros = 300000, .data = {.is_hit = false}});
  replay.events.push_back(
      {.type = ReplayEventType::MOUSE_CLICK, .time_micros = 500000, .data = {.is_hit = false}});
  replay.events.push_back(
      {.type = ReplayEventType::MOUSE_CLICK, .time_micros = 700000, .data = {.is_hit = false}});

  ScenarioAnalysis analysis = AnalyzeScenarioReplay(replay);

  EXPECT_EQ(analysis.clicks, 4);
  EXPECT_EQ(analysis.hits, 1);
  EXPECT_EQ(analysis.misses, 3);
  EXPECT_EQ(analysis.longest_miss_streak, 3);
  EXPECT_FLOAT_EQ(analysis.accuracy_percent, 25.0f);
  EXPECT_NEAR(analysis.average_click_interval_ms, 200.0f, 0.01f);
  EXPECT_FALSE(analysis.findings.empty());
}

TEST(ScenarioAnalysisTest, BuildsCoachPromptFromMeasuredData) {
  ScenarioAnalysis analysis;
  analysis.scenario_name = "Prompt Test";
  analysis.deterministic_summary = "measured summary";
  analysis.timeline_summary = "0.20s MISS";
  analysis.findings.push_back(
      {AnalysisSeverity::WARNING, "Accuracy", "Miss discipline", "Measured miss detail", 0.2f});

  const std::string prompt = BuildNimAnalysisPrompt(analysis);

  EXPECT_NE(prompt.find("AI Overview"), std::string::npos);
  EXPECT_NE(prompt.find("Weak Points"), std::string::npos);
  EXPECT_NE(prompt.find("Measured miss detail"), std::string::npos);
}

}  // namespace
}  // namespace aim
