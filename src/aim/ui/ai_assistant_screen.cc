#include "ai_assistant_screen.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "aim/analysis/nim_client.h"
#include "aim/common/files.h"
#include "aim/common/mat_icons.h"
#include "aim/common/name_util.h"
#include "aim/common/resource_name.h"
#include "aim/common/util.h"
#include "aim/core/bundle_manager.h"
#include "aim/core/playlist_manager.h"
#include "aim/core/scenario_manager.h"
#include "aim/core/stats_manager.h"
#include "aim/proto/playlist.pb.h"
#include "aim/proto/scenario.pb.h"
#include "aim/ui/ui_screen.h"
#include "imgui.h"
#include "imgui/misc/cpp/imgui_stdlib.h"

namespace aim {
namespace {

struct ChatMessage {
  bool user = false;
  std::string text;
};

std::string ExtractLineValue(const std::string& text, const std::string& prefix) {
  const size_t pos = text.find(prefix);
  if (pos == std::string::npos) return {};
  const size_t start = pos + prefix.size();
  const size_t end = text.find('\n', start);
  return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string ExtractJsonObject(const std::string& text, const std::string& prefix) {
  const size_t pos = text.find(prefix);
  if (pos == std::string::npos) return {};
  size_t start = pos + prefix.size();
  while (start < text.size() && (text[start] == ' ' || text[start] == '\n' || text[start] == '\r')) {
    ++start;
  }
  if (start >= text.size() || text[start] != '{') return {};

  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (size_t i = start; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) return text.substr(start, i - start + 1);
    }
  }
  return {};
}

class AiAssistantScreen : public UiScreen {
 public:
  AiAssistantScreen() {
    messages_.push_back({false,
                         "I have access to your scenarios, playlists, statistics, settings, "
                         "replay/analysis features, and writable bundles. I can also create "
                         "scenarios and playlists. Try: "build me a 20-minute tracking playlist "
                         "from my weakest scenarios"."});
  }

 protected:
  void DrawScreen() override {
    ImGui::IdGuard cid("AiAssistantScreen");

    if (app_.BeginFullscreenWindow("AI Coach")) {
      DrawContent();
    }
    ImGui::End();
  }

  void OnTick() override {
    if (request_ && request_->done) {
      std::lock_guard lock(request_->mutex);
      if (request_->success) {
        const std::string response = request_->response;
        messages_.push_back({false, response});
        ExecuteAction(response);
      } else {
        messages_.push_back({false, "AI request failed: " + request_->error});
      }
      request_.reset();
    }
  }

 private:
  void DrawContent() {
    const float char_x = ImGui::GetDefaultCharSizeX();

    ImGui::Text("AI Coach");
    ImGui::SameLine();
    ImGui::TextDisabled("NVIDIA NIM");
    ImGui::Separator();

    if (!IsNimConfigured()) {
      ImGui::TextWrapped(
          "NVIDIA NIM is not configured. Set NVIDIA_NIM_API_KEY and restart FpsAimForge.");
      ImGui::Spacing();
      ImGui::TextWrapped("%s", GetNimConfigurationHint().c_str());
      ImGui::End();
      return;
    }

    if (ImGui::BeginChild("ChatHistory", ImVec2(0, -char_x * 8), true)) {
      for (const auto& message : messages_) {
        ImGui::PushStyleColor(ImGuiCol_Text, message.user ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]
                                                          : ImGui::GetStyle().Colors[ImGuiCol_Text]);
        ImGui::TextWrapped("%s", (message.user ? "You: " : "AI: ").append(message.text).c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
      }
      if (request_) {
        ImGui::TextDisabled("AI is thinking...");
      }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::InputTextMultiline("##AiInput",
                              &input_,
                              ImVec2(-char_x * 9, char_x * 5),
                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(request_ != nullptr || input_.empty());
    if (ImGui::Button("Send", ImVec2(char_x * 7, char_x * 5))) {
      SendMessage();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Back")) {
      PopSelf();
    }
  }

  std::string BuildAppContext(const std::string& user_text) {
    std::string context;
    context += "FpsAimForge live application context. Do not claim capabilities not listed here.\n";
    context += "Writable bundles: ";
    for (const auto& bundle : app_.bundle_manager().GetWritableBundleNames()) {
      context += bundle + ", ";
    }
    context += "\nCurrent scenario: " + app_.scenario_manager().GetCurrentScenarioName() + "\n";
    context += "Current playlist: " + app_.playlist_manager().current_playlist_name() + "\n";

    auto scenario_names = app_.scenario_manager().scenario_names();
    context += std::format("Scenarios ({}):\n", scenario_names ? scenario_names->size() : 0);
    if (scenario_names) {
      for (const auto& name : *scenario_names) {
        auto aggregate = app_.stats_manager().GetAggregateStats(name);
        context += std::format("  {} | runs={} | high_score={:.3f} | last_score={:.3f} | high_cm360={}\n",
                               name,
                               aggregate.total_runs,
                               aggregate.high_score_stats.score,
                               aggregate.last_run_stats.score,
                               aggregate.high_score_stats.mm_per_360);
        if (user_text.find(name) != std::string::npos) {
          auto full_stats = app_.stats_manager().GetStats(name);
          context += std::format("    RAW STATS FOR REQUESTED SCENARIO ({} rows):\n", full_stats.size());
          for (const auto& row : full_stats) {
            context += std::format("      run={} epoch={} score={:.3f} cm360={}\n",
                                   row.stats_id, row.epoch_seconds, row.score, row.mm_per_360);
          }
        }
      }
    }

    auto playlist_names = app_.playlist_manager().playlist_names();
    context += std::format("Playlists ({}):\n", playlist_names ? playlist_names->size() : 0);
    if (playlist_names) {
      for (const auto& name : *playlist_names) {
        auto playlist = app_.playlist_manager().GetPlaylist(name);
        if (!playlist) continue;
        context += std::format("  {} | items={}", name, playlist->items().size());
        for (const auto& item : playlist->items()) {
          context += std::format(" [{} x{}]", item.scenario(), item.num_plays());
        }
        context += "\n";
      }
    }

    const std::string current = app_.scenario_manager().GetCurrentScenarioName();
    if (!current.empty()) {
      auto stats = app_.stats_manager().GetStats(current);
      context += std::format("Full raw stats for current scenario {} ({} runs):\n", current, stats.size());
      for (const auto& row : stats) {
        context += std::format("  run={} epoch={} score={:.3f} cm360={}\n",
                               row.stats_id, row.epoch_seconds, row.score, row.mm_per_360);
      }
    }

    context +=
        "Available app capabilities: start/resume scenarios, inspect scenario definitions, inspect "
        "and edit playlists, inspect all recorded statistics, replay review and visual AI analysis, "
        "scenario analysis, themes/settings/crosshair configuration, bundle save/load, and local "
        "history. This assistant may create scenarios and playlists through validated protobuf JSON.\n";
    return context;
  }

  std::string BuildSystemPrompt() {
    return
        "You are the built-in FpsAimForge AI Coach and application assistant. You have live "
        "application context supplied with every message. Answer directly and accurately. Use "
        "statistics to identify weaknesses and recommend training. Never invent stats, scenarios, "
        "or app capabilities. You may propose actions.\n\n"
        "ACTION PROTOCOL: For a creation/change request, output a normal explanation first, then "
        "one action block. Supported actions are CREATE_SCENARIO, CREATE_PLAYLIST, "
        "ADD_SCENARIO_TO_PLAYLIST, SET_CURRENT_SCENARIO, SET_CURRENT_PLAYLIST, and RUN_CURRENT. "
        "For CREATE_SCENARIO use exactly:\n"
        "ACTION:CREATE_SCENARIO\nNAME:<full bundle/name>\nJSON:<valid ScenarioDef protobuf JSON object>\n"
        "For CREATE_PLAYLIST use exactly:\n"
        "ACTION:CREATE_PLAYLIST\nNAME:<full bundle/name>\nJSON:<valid PlaylistDef protobuf JSON object>\n"
        "For ADD_SCENARIO_TO_PLAYLIST use:\nACTION:ADD_SCENARIO_TO_PLAYLIST\nPLAYLIST:<full name>\nSCENARIO:<full name>\n"
        "For SET_CURRENT_SCENARIO use SCENARIO:<full name>. For SET_CURRENT_PLAYLIST use PLAYLIST:<full name>. "
        "For RUN_CURRENT output ACTION:RUN_CURRENT. Never emit DELETE actions. Creation actions must use a "
        "writable bundle listed in context. Keep generated scenario JSON conservative and valid. Prefer "
        "simple static, linear, strafe, or reference scenarios when the user does not specify a complex type. "
        "If the user asks for a playlist from existing scenarios, use those exact scenario names.\n\n"
        "ScenarioDef JSON follows the application's protobuf field names, for example durationSeconds, "
        "shotType, targetDef, room, staticDef, linearDef, strafeDef, scoreTargets, and description. "
        "PlaylistDef JSON uses description and items, where each item has scenario and numPlays.";
  }

  void SendMessage() {
    if (input_.empty() || request_) return;
    const std::string user_text = input_;
    input_.clear();
    messages_.push_back({true, user_text});

    std::string conversation;
    const size_t begin = messages_.size() > 12 ? messages_.size() - 12 : 0;
    for (size_t i = begin; i < messages_.size(); ++i) {
      conversation += messages_[i].user ? "USER: " : "ASSISTANT: ";
      conversation += messages_[i].text + "\n";
    }
    conversation += "\nLIVE APP CONTEXT:\n" + BuildAppContext();
    request_ = StartNimChat(BuildSystemPrompt(), conversation);
  }

  void ExecuteAction(const std::string& response) {
    const std::string action = ExtractLineValue(response, "ACTION:");
    if (action.empty()) return;

    if (action == "CREATE_SCENARIO") {
      const std::string name = ExtractLineValue(response, "NAME:");
      const std::string json = ExtractJsonObject(response, "JSON:");
      ScenarioDef def;
      if (name.empty() || json.empty() || !JsonToMessage(json, &def)) {
        messages_.push_back({false, "I could not validate the generated ScenarioDef, so nothing was changed."});
        return;
      }
      std::string target_name = name;
      if (target_name.find(':') == std::string::npos) {
        target_name = app_.bundle_manager().GetDefaultWritableBundleName() + ":" + target_name;
      }
      const std::string scenario_bundle = GetNameInfo(target_name).GetBundleName();
      if (scenario_bundle.empty() || app_.bundle_manager().IsBundleReadonly(scenario_bundle)) {
        messages_.push_back({false, "That scenario targets a readonly or invalid bundle. Nothing was changed."});
        return;
      }
      if (GetNameInfo(target_name).HasDynamicSuffix()) {
        messages_.push_back({false, "That scenario name is reserved for an automatic variation."});
        return;
      }
      if (def.duration_seconds() <= 0) def.set_duration_seconds(45);
      const std::string saved = app_.scenario_manager().SaveScenarioWithUniqueName(target_name, def);
      if (saved.empty()) {
        messages_.push_back({false, "The scenario could not be saved."});
        return;
      }
      app_.bundle_manager().SaveDirtyBundles();
      app_.scenario_manager().SetCurrentScenario(saved);
      messages_.push_back({false, "Created scenario: " + saved});
      return;
    }

    if (action == "CREATE_PLAYLIST") {
      const std::string name = ExtractLineValue(response, "NAME:");
      const std::string json = ExtractJsonObject(response, "JSON:");
      PlaylistDef def;
      if (name.empty() || json.empty() || !JsonToMessage(json, &def)) {
        messages_.push_back({false, "I could not validate the generated PlaylistDef, so nothing was changed."});
        return;
      }
      for (auto& item : *def.mutable_items()) {
        if (item.num_plays() <= 0) item.set_num_plays(1);
      }
      std::string target_name = name;
      if (target_name.find(':') == std::string::npos) {
        target_name = app_.bundle_manager().GetDefaultWritableBundleName() + ":" + target_name;
      }
      const std::string playlist_bundle = GetNameInfo(target_name).GetBundleName();
      if (playlist_bundle.empty() || app_.bundle_manager().IsBundleReadonly(playlist_bundle)) {
        messages_.push_back({false, "That playlist targets a readonly or invalid bundle. Nothing was changed."});
        return;
      }
      if (app_.playlist_manager().GetPlaylist(target_name).has_value()) {
        messages_.push_back({false, "That playlist already exists. I will not overwrite it automatically."});
        return;
      }
      app_.playlist_manager().UpdatePlaylist(target_name, def);
      app_.bundle_manager().SaveDirtyBundles();
      app_.playlist_manager().SetCurrentPlaylist(target_name);
      messages_.push_back({false, "Created playlist: " + target_name});
      return;
    }

    if (action == "ADD_SCENARIO_TO_PLAYLIST") {
      const std::string playlist = ExtractLineValue(response, "PLAYLIST:");
      const std::string scenario = ExtractLineValue(response, "SCENARIO:");
      if (playlist.empty() || scenario.empty()) {
        messages_.push_back({false, "The add-to-playlist action was incomplete, so nothing changed."});
        return;
      }
      app_.playlist_manager().AddScenarioToPlaylist(playlist, scenario);
      app_.bundle_manager().SaveDirtyBundles();
      messages_.push_back({false, std::format("Added {} to {}.", scenario, playlist)});
      return;
    }

    if (action == "SET_CURRENT_SCENARIO") {
      const std::string scenario = ExtractLineValue(response, "SCENARIO:");
      if (!scenario.empty() && app_.scenario_manager().SetCurrentScenario(scenario)) {
        messages_.push_back({false, "Current scenario set to " + scenario});
      }
      return;
    }

    if (action == "SET_CURRENT_PLAYLIST") {
      const std::string playlist = ExtractLineValue(response, "PLAYLIST:");
      if (!playlist.empty()) {
        app_.playlist_manager().SetCurrentPlaylist(playlist);
        messages_.push_back({false, "Current playlist set to " + playlist});
      }
      return;
    }

    if (action == "RUN_CURRENT") {
      app_.state().scenario_run_option = ScenarioRunOption::START_CURRENT;
      ReturnHome();
      return;
    }
  }

  std::vector<ChatMessage> messages_;
  std::string input_;
  std::shared_ptr<NimAnalysisState> request_;
};

}  // namespace

std::shared_ptr<Screen> CreateAiAssistantScreen() {
  return std::make_shared<AiAssistantScreen>();
}

}  // namespace aim
