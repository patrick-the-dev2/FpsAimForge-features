#include "aim/scenario/replay.h"

#include <cmath>
#include <string>

#include "gtest/gtest.h"

namespace aim {
namespace {

Replay MakeReplay() {
  Replay replay;
  replay.scenario_name = "Persistence Test";
  replay.shot_type = ShotType::kClickSingle;
  replay.replay_fps = 120;
  replay.num_targets = 2;
  replay.cm_per_360 = 17.375f;
  replay.room.set_horizontal_fov(103.0f);
  replay.room.mutable_camera_position()->set_x(1.0f);
  replay.room.mutable_camera_position()->set_y(2.0f);
  replay.room.mutable_camera_position()->set_z(3.0f);

  ReplayEvent click;
  click.type = ReplayEventType::MOUSE_CLICK;
  click.time_micros = 123456;
  click.data.is_hit = true;
  replay.events.push_back(click);

  ReplayEvent remove;
  remove.type = ReplayEventType::REMOVE_TARGET;
  remove.time_micros = 234567;
  remove.data.target_id = 7;
  replay.events.push_back(remove);

  ReplayEvent sound;
  sound.type = ReplayEventType::PLAY_SOUND;
  sound.time_micros = 345678;
  sound.data.play_sound.sound = SoundType::CLICK_HIT;
  replay.events.push_back(sound);

  ReplayTargetData target;
  target.radius = 1.25f;
  target.position = {4.0f, 5.0f, 6.0f};
  target.health = 250;
  replay.target_data.push_back(target);

  replay.pitch_yaws.push_back({0.25f, -0.5f});
  replay.pitch_yaws.push_back({0.5f, 0.75f});
  replay.scores = {0.0f, 12.5f};

  ReplayTargetMetadata metadata{};
  metadata.add_time_micros = 1000;
  metadata.target_id = 7;
  metadata.data_channel = 1;
  metadata.initial_data = target;
  metadata.pill_height = 2.5f;
  metadata.is_ghost = true;
  metadata.has_health = true;
  replay.target_metadata.push_back(metadata);

  return replay;
}

TEST(ReplayTest, RoundTripsPersistedReplay) {
  const Replay original = MakeReplay();
  const std::string data = SerializeReplay(original);
  ASSERT_FALSE(data.empty());

  const auto decoded = DeserializeReplay(data);
  ASSERT_NE(decoded, nullptr);

  EXPECT_EQ(decoded->scenario_name, original.scenario_name);
  EXPECT_EQ(decoded->shot_type, original.shot_type);
  EXPECT_EQ(decoded->replay_fps, original.replay_fps);
  EXPECT_EQ(decoded->num_targets, original.num_targets);
  EXPECT_FLOAT_EQ(decoded->cm_per_360, original.cm_per_360);
  EXPECT_EQ(decoded->events.size(), original.events.size());
  EXPECT_EQ(decoded->target_data.size(), original.target_data.size());
  EXPECT_EQ(decoded->pitch_yaws.size(), original.pitch_yaws.size());
  EXPECT_EQ(decoded->target_metadata.size(), original.target_metadata.size());
  EXPECT_EQ(decoded->scores.size(), original.scores.size());
  EXPECT_FLOAT_EQ(decoded->target_data[0].radius, original.target_data[0].radius);
  EXPECT_FLOAT_EQ(decoded->target_data[0].position.x, original.target_data[0].position.x);
  EXPECT_EQ(decoded->target_data[0].health, original.target_data[0].health);
  EXPECT_EQ(decoded->events[0].type, ReplayEventType::MOUSE_CLICK);
  EXPECT_TRUE(decoded->events[0].data.is_hit);
  EXPECT_EQ(decoded->events[1].data.target_id, 7);
  EXPECT_EQ(decoded->events[2].data.play_sound.sound, SoundType::CLICK_HIT);
  EXPECT_FLOAT_EQ(decoded->target_metadata[0].pill_height, 2.5f);
  EXPECT_TRUE(decoded->target_metadata[0].is_ghost);
  EXPECT_TRUE(decoded->target_metadata[0].has_health);
}

TEST(ReplayTest, RejectsCorruptPayload) {
  const Replay original = MakeReplay();
  const std::string data = SerializeReplay(original);
  ASSERT_GT(data.size(), 10U);

  std::string corrupt = data;
  corrupt[0] = 'X';
  EXPECT_EQ(DeserializeReplay(corrupt), nullptr);

  corrupt = data.substr(0, data.size() - 1);
  EXPECT_EQ(DeserializeReplay(corrupt), nullptr);
}

}  // namespace
}  // namespace aim
