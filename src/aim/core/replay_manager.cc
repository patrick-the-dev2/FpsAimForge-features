#include "replay_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "aim/common/log.h"
#include "aim/database/aim_db.h"
#include "aim/scenario/replay.h"

namespace aim {
namespace {

constexpr const int kMaxSmallReplays = 20;

struct ReplayEntry {
  i64 run_id = -1;
  std::shared_ptr<Replay> replay;
};

class ReplayManagerImpl : public ReplayManager {
 public:
  explicit ReplayManagerImpl(AimDb* db) : db_(db) {}
  std::shared_ptr<Replay> GetReplay(i64 run_id) override {
    if (large_replay_.run_id == run_id) {
      return large_replay_.replay;
    }
    for (const ReplayEntry& entry : small_replays_) {
      if (entry.run_id == run_id) {
        return entry.replay;
      }
    }

    if (db_ == nullptr) {
      return nullptr;
    }
    const std::string data = db_->GetReplay(run_id);
    if (data.empty()) {
      return nullptr;
    }
    auto replay = DeserializeReplay(data);
    if (!replay) {
      return nullptr;
    }
    CacheReplay(run_id, replay);
    return replay;
  }

  void AddReplay(i64 run_id, std::shared_ptr<Replay> replay) override {
    if (!replay) {
      return;
    }

    if (db_ != nullptr) {
      const std::string data = SerializeReplay(*replay);
      if (data.empty() || !db_->AddReplay(run_id, data)) {
        Logger::get()->warn("Failed to persist replay for run {}", run_id);
      }
    }

    CacheReplay(run_id, std::move(replay));
  }

 private:
  void CacheReplay(i64 run_id, std::shared_ptr<Replay> replay) {
    float approximate_size_mb = replay->GetApproximateSizeMb();
    bool is_large = approximate_size_mb > 0.2;
    ReplayEntry entry;
    entry.run_id = run_id;
    entry.replay = std::move(replay);

    if (is_large) {
      large_replay_ = std::move(entry);
      return;
    }

    if (small_replays_.size() >= kMaxSmallReplays) {
      small_replays_.erase(small_replays_.begin());
    }
    small_replays_.push_back(std::move(entry));
  }

  AimDb* db_;
  std::vector<ReplayEntry> small_replays_;
  ReplayEntry large_replay_;
};

}  // namespace

std::unique_ptr<ReplayManager> CreateReplayManager(AimDb* db) {
  return std::make_unique<ReplayManagerImpl>(db);
}

}  // namespace aim