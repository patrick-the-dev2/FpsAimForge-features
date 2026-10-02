#include "nim_client.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

namespace aim {
namespace {

constexpr const char* kDefaultEndpoint =
    "https://integrate.api.nvidia.com/v1/chat/completions";
constexpr const char* kDefaultModel = "deepseek-ai/deepseek-v4-flash";
std::atomic_uint64_t g_request_counter{0};

std::string GetEnv(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? "" : value;
}

std::string JsonEscape(const std::string& value) {
  std::string result;
  for (unsigned char c : value) {
    switch (c) {
      case '\"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result += c < 0x20 ? ' ' : static_cast<char>(c);
    }
  }
  return result;
}

std::string CurlConfigEscape(const std::string& value) {
  std::string result;
  for (char c : value) {
    if (c == '\\' || c == '\"') result += '\\';
    result += c;
  }
  return result;
}

std::string ExtractJsonString(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) return "";

  size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return "";
  size_t start = json.find('\"', colon + 1);
  if (start == std::string::npos) return "";

  std::string result;
  bool escaped = false;
  for (size_t i = start + 1; i < json.size(); ++i) {
    const char c = json[i];
    if (escaped) {
      switch (c) {
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case '\"': result += '\"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        default: result += c; break;
      }
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '\"') {
      return result;
    } else {
      result += c;
    }
  }
  return "";
}

}  // namespace

bool IsNimConfigured() {
  return !GetEnv("NVIDIA_NIM_API_KEY").empty() ||
         !GetEnv("NVIDIA_API_KEY").empty() ||
         !GetEnv("NVIDIA_NIM_ENDPOINT").empty();
}

std::string GetNimConfigurationHint() {
  return "Set NVIDIA_NIM_API_KEY for NVIDIA's hosted endpoint, or set "
         "NVIDIA_NIM_ENDPOINT for a self-hosted NIM. NVIDIA_NIM_MODEL can "
         "override the model identifier.";
}

std::shared_ptr<NimAnalysisState> StartNimAnalysis(const std::string& prompt) {
  auto state = std::make_shared<NimAnalysisState>();

  const std::string api_key =
      !GetEnv("NVIDIA_NIM_API_KEY").empty() ? GetEnv("NVIDIA_NIM_API_KEY")
                                            : GetEnv("NVIDIA_API_KEY");
  const std::string endpoint =
      !GetEnv("NVIDIA_NIM_ENDPOINT").empty() ? GetEnv("NVIDIA_NIM_ENDPOINT")
                                             : kDefaultEndpoint;
  const std::string model =
      !GetEnv("NVIDIA_NIM_MODEL").empty() ? GetEnv("NVIDIA_NIM_MODEL") : kDefaultModel;

  std::thread([state, prompt, api_key, endpoint, model]() {
    const auto id = g_request_counter.fetch_add(1);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
                      std::format("fpsaimforge_nim_{}_{}", stamp, id);
    const auto payload_path = base.string() + ".json";
    const auto response_path = base.string() + ".response";
    const auto config_path = base.string() + ".curl";

    std::error_code ec;
    {
      std::ofstream payload(payload_path);
      payload << "{\"model\":\"" << JsonEscape(model)
              << "\",\"messages\":[{\"role\":\"system\",\"content\":\"You are a precise "
                 "FPS aim coach. Use only measured replay facts.\"},{\"role\":\"user\",\"content\":\""
              << JsonEscape(prompt)
              << "\"}],\"max_tokens\":1200,\"temperature\":0.2,\"stream\":false}";
    }
    {
      std::ofstream config(config_path);
      config << "url = \"" << CurlConfigEscape(endpoint) << "\"\n";
      config << "request = \"POST\"\n";
      config << "header = \"Accept: application/json\"\n";
      config << "header = \"Content-Type: application/json\"\n";
      if (!api_key.empty()) {
        config << "header = \"Authorization: Bearer " << CurlConfigEscape(api_key) << "\"\n";
      }
      config << "data-binary = \"@" << CurlConfigEscape(payload_path) << "\"\n";
      config << "output = \"" << CurlConfigEscape(response_path) << "\"\n";
    }

    const std::string command =
        "curl --silent --show-error --fail --max-time 60 --config \"" +
        config_path + "\"";
    const int exit_code = std::system(command.c_str());

    std::string response;
    if (exit_code == 0) {
      std::ifstream input(response_path);
      response.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    std::filesystem::remove(payload_path, ec);
    std::filesystem::remove(config_path, ec);
    std::filesystem::remove(response_path, ec);

    std::lock_guard lock(state->mutex);
    state->done = true;
    if (exit_code != 0) {
      state->error =
          "NVIDIA NIM request failed. Check the API key, endpoint, model, and network connection.";
      return;
    }

    state->response = ExtractJsonString(response, "content");
    state->success = !state->response.empty();
    if (!state->success) {
      state->error = "NVIDIA NIM returned no assistant content.";
    }
  }).detach();

  return state;
}

}  // namespace aim
