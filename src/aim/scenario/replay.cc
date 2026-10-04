#include "replay.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "aim/core/target.h"

namespace aim {

ReplayRecorder::ReplayRecorder(const std::string& scenario_name,
                               const Room& room,
                               ShotType::TypeCase shot_type,
                               u16 replay_fps,
                               i32 duration_seconds,
                               i32 num_targets,
                               bool requires_per_frame_target_data)
    : num_targets_(num_targets) {
  replay_ = std::make_shared<Replay>();
  replay_->scenario_name = scenario_name;
  replay_->room = room;
  replay_->replay_fps = static_cast<u16>(replay_fps);
  replay_->num_targets = static_cast<u16>(num_targets);
  replay_->shot_type = shot_type;

  i32 max_replay_frame_number = replay_fps * duration_seconds;
  i32 total_target_data_count = max_replay_frame_number * num_targets;

  if (requires_per_frame_target_data) {
    ReplayTargetData invalid_target_data;
    invalid_target_data.radius = -1;
    replay_->target_data.resize(total_target_data_count, invalid_target_data);
  }

  PitchYaw invalid_pitch_yaw;
  invalid_pitch_yaw.pitch = GetMaxPitch() * 3;
  replay_->pitch_yaws.resize(max_replay_frame_number, invalid_pitch_yaw);

  // Make this large enough that it reasonably won't need to allocate more memory during a run.
  // Maybe we should drop certain event types (like sound) if it is approaching the limit.
  replay_->events.reserve(3000);
  replay_->target_metadata.reserve(500);

  replay_->scores.reserve(130 * kRecordScoresPerSecond);
}

void ReplayRecorder::FillInMissingPitchYaws() {
  float max_pitch = GetMaxPitch();

  auto find_first_valid = [=, this](int i) {
    for (; i < replay_->pitch_yaws.size(); ++i) {
      PitchYaw& pitch_yaw = replay_->pitch_yaws[i];
      bool is_invalid = pitch_yaw.pitch > max_pitch;
      if (!is_invalid) {
        return pitch_yaw;
      }
    }
    return PitchYaw{};
  };

  for (int i = 0; i < replay_->pitch_yaws.size(); ++i) {
    PitchYaw& pitch_yaw = replay_->pitch_yaws[i];
    bool is_invalid = pitch_yaw.pitch > max_pitch;
    if (is_invalid) {
      if (i == 0) {
        pitch_yaw = find_first_valid(i + 1);
      } else {
        const PitchYaw& prev = replay_->pitch_yaws[i - 1];
        pitch_yaw.pitch = prev.pitch;
        pitch_yaw.yaw = prev.yaw;
      }
    }
  }
}

void ReplayRecorder::AddTarget(i64 now_micros, const Target& target) {
  // Find available data channel.
  std::vector<bool> taken_channels(num_targets_, false);
  for (auto& entry : target_data_channel_map_) {
    taken_channels[entry.second] = true;
  }

  int available_channel = 0;
  for (; available_channel < num_targets_; ++available_channel) {
    if (!taken_channels[available_channel]) {
      break;
    }
  }

  target_data_channel_map_[target.id] = static_cast<u16>(available_channel);

  replay_->target_metadata.push_back({});
  ReplayTargetMetadata& metadata = replay_->target_metadata.back();
  metadata.add_time_micros = now_micros;
  metadata.target_id = target.id;
  metadata.data_channel = static_cast<u16>(available_channel);
  metadata.initial_data.position = target.position;
  metadata.initial_data.radius = target.radius;
  metadata.is_ghost = target.is_ghost;
  if (target.is_pill) {
    metadata.pill_height = target.height;
  }
  if (target.health_seconds > 0) {
    metadata.has_health = true;
  }
}

void ReplayRecorder::PlaySound(i64 now_micros, SoundType sound) {
  ReplayEvent& event = AddEvent(now_micros, ReplayEventType::PLAY_SOUND);
  event.data.play_sound.sound = sound;
}

void ReplayRecorder::RemoveTarget(i64 now_micros, u16 target_id) {
  ReplayEvent& event = AddEvent(now_micros, ReplayEventType::REMOVE_TARGET);
  event.data.target_id = target_id;
  target_data_channel_map_.erase(target_id);
}

void ReplayRecorder::AddMouseClick(i64 now_micros, bool is_hit) {
  ReplayEvent& event = AddEvent(now_micros, ReplayEventType::MOUSE_CLICK);
  event.data.is_hit = is_hit;
}

void ReplayRecorder::AddScore(float score) {
  replay_->scores.push_back(score);
}

ReplayEvent& ReplayRecorder::AddEvent(i64 now_micros, ReplayEventType type) {
  replay_->events.push_back({});
  ReplayEvent& event = replay_->events.back();
  event.time_micros = now_micros;
  event.type = type;
  return event;
}

void ReplayRecorder::SetPitchYaw(i64 frame_number, float pitch, float yaw) {
  if (frame_number < replay_->pitch_yaws.size()) {
    PitchYaw& val = replay_->pitch_yaws[frame_number];
    val.pitch = pitch;
    val.yaw = yaw;
  }
}

void ReplayRecorder::SnapshotTargets(i64 frame_number, const std::vector<Target>& targets) {
  int start_index = frame_number * num_targets_;
  int end_index = start_index + num_targets_;

  bool is_valid_max_index = end_index <= replay_->target_data.size();
  if (!is_valid_max_index) {
    return;
  }

  assert(targets.size() <= num_targets_ && "Too many targets");
  for (const Target& target : targets) {
    if (target.hidden) {
      continue;
    }
    auto it = target_data_channel_map_.find(target.id);
    if (it != target_data_channel_map_.end()) {
      u16 data_channel = it->second;
      assert(data_channel < num_targets_ && "Invalid data channel");
      if (data_channel < num_targets_) {
        ReplayTargetData& data = replay_->target_data[start_index + data_channel];
        data.position = target.position;
        data.radius = target.radius;
        data.health = static_cast<u8>(std::round(target.GetHealthPercent() * 255));
      }
    }
  }
}


namespace {

constexpr std::string_view kReplayMagic = "FPSAIMFORGE_REPLAY_V1";
constexpr u32 kMaxStringBytes = 1U * 1024U * 1024U;
constexpr u32 kMaxRoomBytes = 1U * 1024U * 1024U;
constexpr u32 kMaxVectorElements = 2U * 1024U * 1024U;

class ReplayWriter {
 public:
  void WriteU8(u8 value) { data_.push_back(static_cast<char>(value)); }

  void WriteU16(u16 value) {
    data_.push_back(static_cast<char>(value & 0xff));
    data_.push_back(static_cast<char>((value >> 8) & 0xff));
  }

  void WriteU32(u32 value) {
    for (int shift = 0; shift < 32; shift += 8) {
      data_.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  }

  void WriteF32(float value) {
    u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(bits);
  }

  void WriteString(std::string_view value) {
    WriteU32(static_cast<u32>(value.size()));
    data_.append(value);
  }

  void WriteVec3(const glm::vec3& value) {
    WriteF32(value.x);
    WriteF32(value.y);
    WriteF32(value.z);
  }

  void AppendRaw(std::string_view value) { data_.append(value); }

  std::string Take() { return std::move(data_); }

 private:
  std::string data_;
};

class ReplayReader {
 public:
  explicit ReplayReader(std::string_view data) : data_(data) {}

  bool ReadU8(u8* out) {
    if (remaining() < 1) return false;
    *out = static_cast<u8>(static_cast<unsigned char>(data_[position_]));
    ++position_;
    return true;
  }

  bool ReadU16(u16* out) {
    if (remaining() < 2) return false;
    *out = static_cast<u16>(static_cast<unsigned char>(data_[position_])) |
           static_cast<u16>(static_cast<unsigned char>(data_[position_ + 1]) << 8);
    position_ += 2;
    return true;
  }

  bool ReadU32(u32* out) {
    if (remaining() < 4) return false;
    *out = static_cast<u32>(static_cast<unsigned char>(data_[position_])) |
           (static_cast<u32>(static_cast<unsigned char>(data_[position_ + 1])) << 8) |
           (static_cast<u32>(static_cast<unsigned char>(data_[position_ + 2])) << 16) |
           (static_cast<u32>(static_cast<unsigned char>(data_[position_ + 3])) << 24);
    position_ += 4;
    return true;
  }

  bool ReadF32(float* out) {
    u32 bits = 0;
    if (!ReadU32(&bits)) return false;
    static_assert(sizeof(bits) == sizeof(*out));
    std::memcpy(out, &bits, sizeof(bits));
    return std::isfinite(*out);
  }

  bool ReadString(std::string* out, u32 max_bytes) {
    u32 size = 0;
    if (!ReadU32(&size) || size > max_bytes || remaining() < size) return false;
    out->assign(data_.substr(position_, size));
    position_ += size;
    return true;
  }

  bool ReadRaw(size_t size, std::string* out) {
    if (remaining() < size) return false;
    out->assign(data_.substr(position_, size));
    position_ += size;
    return true;
  }

  bool ReadVec3(glm::vec3* out) {
    return ReadF32(&out->x) && ReadF32(&out->y) && ReadF32(&out->z);
  }

  bool Empty() const { return position_ == data_.size(); }

 private:
  size_t remaining() const { return data_.size() - position_; }

  std::string_view data_;
  size_t position_ = 0;
};

template <typename T>
bool ReadCount(ReplayReader* reader, std::vector<T>* vector) {
  u32 count = 0;
  if (!reader->ReadU32(&count) || count > kMaxVectorElements) return false;
  vector->clear();
  vector->resize(count);
  return true;
}

bool SerializeReplayEvent(ReplayWriter* writer, const ReplayEvent& event) {
  writer->WriteU16(static_cast<u16>(event.type));
  writer->WriteU32(event.time_micros);
  switch (event.type) {
    case ReplayEventType::PLAY_SOUND:
      writer->WriteU16(static_cast<u16>(event.data.play_sound.sound));
      return true;
    case ReplayEventType::REMOVE_TARGET:
      writer->WriteU16(event.data.target_id);
      return true;
    case ReplayEventType::MOUSE_CLICK:
      writer->WriteU8(event.data.is_hit ? 1 : 0);
      return true;
  }
  return false;
}

bool DeserializeReplayEvent(ReplayReader* reader, ReplayEvent* event) {
  u16 type = 0;
  if (!reader->ReadU16(&type) || !reader->ReadU32(&event->time_micros)) return false;
  event->type = static_cast<ReplayEventType>(type);
  switch (event->type) {
    case ReplayEventType::PLAY_SOUND: {
      u16 sound = 0;
      if (!reader->ReadU16(&sound)) return false;
      event->data.play_sound.sound = static_cast<SoundType>(sound);
      return true;
    }
    case ReplayEventType::REMOVE_TARGET:
      return reader->ReadU16(&event->data.target_id);
    case ReplayEventType::MOUSE_CLICK: {
      u8 is_hit = 0;
      if (!reader->ReadU8(&is_hit) || is_hit > 1) return false;
      event->data.is_hit = is_hit != 0;
      return true;
    }
  }
  return false;
}

bool SerializeReplayTargetData(ReplayWriter* writer, const ReplayTargetData& data) {
  writer->WriteF32(data.radius);
  writer->WriteVec3(data.position);
  writer->WriteU8(data.health);
  return true;
}

bool DeserializeReplayTargetData(ReplayReader* reader, ReplayTargetData* data) {
  return reader->ReadF32(&data->radius) &&
         reader->ReadVec3(&data->position) &&
         reader->ReadU8(&data->health);
}

bool SerializeReplayMetadata(ReplayWriter* writer, const ReplayTargetMetadata& data) {
  writer->WriteU32(data.add_time_micros);
  writer->WriteU16(data.target_id);
  writer->WriteU16(data.data_channel);
  if (!SerializeReplayTargetData(writer, data.initial_data)) return false;
  writer->WriteF32(data.pill_height);
  writer->WriteU8(data.is_ghost ? 1 : 0);
  writer->WriteU8(data.has_health ? 1 : 0);
  return true;
}

bool DeserializeReplayMetadata(ReplayReader* reader, ReplayTargetMetadata* data) {
  u8 is_ghost = 0;
  u8 has_health = 0;
  return reader->ReadU32(&data->add_time_micros) &&
         reader->ReadU16(&data->target_id) &&
         reader->ReadU16(&data->data_channel) &&
         DeserializeReplayTargetData(reader, &data->initial_data) &&
         reader->ReadF32(&data->pill_height) &&
         reader->ReadU8(&is_ghost) &&
         reader->ReadU8(&has_health) &&
         is_ghost <= 1 && has_health <= 1 &&
         (data->is_ghost = is_ghost != 0, data->has_health = has_health != 0, true);
}

}  // namespace

std::string SerializeReplay(const Replay& replay) {
  if (replay.replay_fps == 0 || replay.num_targets == 0) return {};
  if (replay.scenario_name.size() > kMaxStringBytes) return {};
  const std::string room_data = replay.room.SerializeAsString();
  if (room_data.size() > kMaxRoomBytes ||
      replay.events.size() > kMaxVectorElements ||
      replay.target_data.size() > kMaxVectorElements ||
      replay.pitch_yaws.size() > kMaxVectorElements ||
      replay.target_metadata.size() > kMaxVectorElements ||
      replay.scores.size() > kMaxVectorElements) {
    return {};
  }

  ReplayWriter writer;
  writer.AppendRaw(kReplayMagic);
  writer.WriteU32(1);
  writer.WriteString(replay.scenario_name);
  writer.WriteString(room_data);
  writer.WriteU16(static_cast<u16>(replay.shot_type));
  writer.WriteU16(replay.replay_fps);
  writer.WriteU16(replay.num_targets);
  writer.WriteF32(replay.cm_per_360);

  writer.WriteU32(static_cast<u32>(replay.events.size()));
  for (const auto& event : replay.events) {
    if (!SerializeReplayEvent(&writer, event)) return {};
  }

  writer.WriteU32(static_cast<u32>(replay.target_data.size()));
  for (const auto& data : replay.target_data) {
    SerializeReplayTargetData(&writer, data);
  }

  writer.WriteU32(static_cast<u32>(replay.pitch_yaws.size()));
  for (const auto& pitch_yaw : replay.pitch_yaws) {
    writer.WriteF32(pitch_yaw.pitch);
    writer.WriteF32(pitch_yaw.yaw);
  }

  writer.WriteU32(static_cast<u32>(replay.target_metadata.size()));
  for (const auto& metadata : replay.target_metadata) {
    if (!SerializeReplayMetadata(&writer, metadata)) return {};
  }

  writer.WriteU32(static_cast<u32>(replay.scores.size()));
  for (float score : replay.scores) {
    writer.WriteF32(score);
  }

  return writer.Take();
}

std::shared_ptr<Replay> DeserializeReplay(std::string_view data) {
  if (data.size() < kReplayMagic.size() + 4) return nullptr;
  ReplayReader reader(data);

  std::string magic;
  if (!reader.ReadRaw(kReplayMagic.size(), &magic) || magic != kReplayMagic) return nullptr;

  u32 version = 0;
  if (!reader.ReadU32(&version) || version != 1) return nullptr;

  auto replay = std::make_shared<Replay>();
  std::string room_data;
  if (!reader.ReadString(&replay->scenario_name, kMaxStringBytes) ||
      !reader.ReadString(&room_data, kMaxRoomBytes)) {
    return nullptr;
  }
  if (!replay->room.ParseFromArray(room_data.data(), static_cast<int>(room_data.size()))) {
    return nullptr;
  }

  u16 shot_type = 0;
  if (!reader.ReadU16(&shot_type) ||
      !reader.ReadU16(&replay->replay_fps) ||
      !reader.ReadU16(&replay->num_targets) ||
      !reader.ReadF32(&replay->cm_per_360)) {
    return nullptr;
  }
  replay->shot_type = static_cast<ShotType::TypeCase>(shot_type);
  if (replay->replay_fps == 0 || replay->num_targets == 0 || replay->cm_per_360 < 0) return nullptr;

  if (!ReadCount(&reader, &replay->events)) return nullptr;
  for (auto& event : replay->events) {
    if (!DeserializeReplayEvent(&reader, &event)) return nullptr;
  }

  if (!ReadCount(&reader, &replay->target_data)) return nullptr;
  for (auto& target_data : replay->target_data) {
    if (!DeserializeReplayTargetData(&reader, &target_data)) return nullptr;
  }

  if (!ReadCount(&reader, &replay->pitch_yaws)) return nullptr;
  for (auto& pitch_yaw : replay->pitch_yaws) {
    if (!reader.ReadF32(&pitch_yaw.pitch) || !reader.ReadF32(&pitch_yaw.yaw)) return nullptr;
  }

  if (!ReadCount(&reader, &replay->target_metadata)) return nullptr;
  for (auto& metadata : replay->target_metadata) {
    if (!DeserializeReplayMetadata(&reader, &metadata)) return nullptr;
  }

  if (!ReadCount(&reader, &replay->scores)) return nullptr;
  for (float& score : replay->scores) {
    if (!reader.ReadF32(&score)) return nullptr;
  }

  if (!reader.Empty()) return nullptr;
  return replay;
}

float Replay::GetApproximateSizeMb() const {
  i64 size_bytes = pitch_yaws.size() * sizeof(PitchYaw);
  size_bytes += target_data.size() * sizeof(ReplayTargetData);
  size_bytes += target_metadata.size() * sizeof(ReplayTargetMetadata);
  size_bytes += events.size() * sizeof(ReplayEvent);
  size_bytes += scores.size() * sizeof(float);

  return size_bytes / 1000000.0f;
}

float Replay::GetDurationSeconds() const {
  return pitch_yaws.size() / static_cast<float>(replay_fps);
}

}  // namespace aim
