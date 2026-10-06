#include "nim_client.h"

#include "SDL3/SDL.h"  // IWYU pragma: keep

#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace aim {
namespace {

constexpr const char* kDefaultEndpoint =
    "https://integrate.api.nvidia.com/v1/chat/completions";
constexpr const char* kDefaultModel = "deepseek-ai/deepseek-v4.1-flash";
std::atomic_uint64_t g_request_counter{0};

std::string GetEnv(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? "" : value;
#endif
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

std::string ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};

  input.seekg(0, std::ios::end);
  const std::streampos size = input.tellg();
  if (size < 0) return {};

  std::string content(static_cast<std::size_t>(size), '\0');
  input.seekg(0, std::ios::beg);
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (!input && !input.eof()) return {};

  content.resize(static_cast<std::size_t>(input.gcount()));
  return content;
}

std::string CurlConfigEscape(const std::string& value) {
  std::string result;
  for (char c : value) {
    if (c == '\\' || c == '"') result += '\\';
    result += c;
  }
  return result;
}

#ifdef _WIN32
int RunCommandNoWindow(const std::string& command) {
  std::string mutable_command = command;
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}
#else
int RunCommandNoWindow(const std::string& command) {
  return std::system(command.c_str());
}
#endif

std::string ExtractJsonString(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) return "";

  size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return "";
  size_t start = json.find('"', colon + 1);
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
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        default: result += c; break;
      }
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
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

std::shared_ptr<NimAnalysisState> StartNimChat(const std::string& system_prompt,
                                                const std::string& conversation_prompt) {
  auto state = std::make_shared<NimAnalysisState>();

  const std::string api_key =
      !GetEnv("NVIDIA_NIM_API_KEY").empty() ? GetEnv("NVIDIA_NIM_API_KEY")
                                            : GetEnv("NVIDIA_API_KEY");
  const std::string endpoint =
      !GetEnv("NVIDIA_NIM_ENDPOINT").empty() ? GetEnv("NVIDIA_NIM_ENDPOINT")
                                             : kDefaultEndpoint;
  const std::string model =
      !GetEnv("NVIDIA_NIM_MODEL").empty() ? GetEnv("NVIDIA_NIM_MODEL") : kDefaultModel;

  std::thread([state, system_prompt, conversation_prompt, api_key, endpoint, model]() {
    const auto id = g_request_counter.fetch_add(1);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
                      std::format("fpsaimforge_nim_chat_{}_{}", stamp, id);
    const auto payload_path = base.string() + ".json";
    const auto response_path = base.string() + ".response";
    const auto config_path = base.string() + ".curl";

    std::error_code ec;
    {
      std::ofstream payload(payload_path);
      payload << "{\"model\":\"" << JsonEscape(model)
              << "\",\"messages\":[{\"role\":\"system\",\"content\":\""
              << JsonEscape(system_prompt)
              << "\"},{\"role\":\"user\",\"content\":\""
              << JsonEscape(conversation_prompt)
              << "\"}],\"max_tokens\":3000,\"temperature\":0.2,\"stream\":false}";
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
        "curl --silent --show-error --fail --max-time 120 --config \"" +
        config_path + "\"";
    const int exit_code = RunCommandNoWindow(command);

    std::string response;
    if (exit_code == 0) response = ReadBinaryFile(response_path);

    std::filesystem::remove(payload_path, ec);
    std::filesystem::remove(config_path, ec);
    std::filesystem::remove(response_path, ec);

    std::lock_guard lock(state->mutex);
    state->done = true;
    if (exit_code != 0) {
      state->error =
          "NVIDIA NIM request failed. Check the API key, model, endpoint, and network connection.";
      return;
    }

    state->response = ExtractJsonString(response, "content");
    state->success = !state->response.empty();
    if (!state->success) {
      const std::string api_error = ExtractJsonString(response, "message");
      const std::string error_detail = ExtractJsonString(response, "detail");
      if (!api_error.empty()) {
        state->error = "NVIDIA NIM rejected the request: " + api_error;
        if (!error_detail.empty()) state->error += " (" + error_detail + ")";
      } else {
        state->error = "NVIDIA NIM returned no assistant content.";
      }
    }
  }).detach();

  return state;
}

std::shared_ptr<NimAnalysisState> StartNimAnalysis(const std::string& prompt) {
  return StartNimChat(
      "You are a precise FPS aim coach. Use only measured replay facts and do not invent events.",
      prompt);
}

namespace {
std::string Base64Encode(const std::string& input) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);
  for (size_t i = 0; i < input.size(); i += 3) {
    const unsigned int a = static_cast<unsigned char>(input[i]);
    const unsigned int b =
        i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
    const unsigned int d =
        i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
    const unsigned int value = (a << 16) | (b << 8) | d;
    output.push_back(kTable[(value >> 18) & 63]);
    output.push_back(kTable[(value >> 12) & 63]);
    output.push_back(i + 1 < input.size() ? kTable[(value >> 6) & 63] : '=');
    output.push_back(i + 2 < input.size() ? kTable[value & 63] : '=');
  }
  return output;
}
}  // namespace

std::shared_ptr<NimAnalysisState> StartNimVisualAnalysis(
    const std::string& prompt, const std::vector<std::filesystem::path>& image_paths) {
  auto state = std::make_shared<NimAnalysisState>();

  const std::string api_key =
      !GetEnv("NVIDIA_NIM_API_KEY").empty() ? GetEnv("NVIDIA_NIM_API_KEY")
                                            : GetEnv("NVIDIA_API_KEY");
  const std::string endpoint =
      !GetEnv("NVIDIA_NIM_ENDPOINT").empty() ? GetEnv("NVIDIA_NIM_ENDPOINT")
                                             : kDefaultEndpoint;
  const std::string model =
      !GetEnv("NVIDIA_NIM_VISION_MODEL").empty()
          ? GetEnv("NVIDIA_NIM_VISION_MODEL")
          : "deepseek-ai/deepseek-v4.1-flash";

  std::thread([state, prompt, image_paths, api_key, endpoint, model]() {
    const auto id = g_request_counter.fetch_add(1);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
                      std::format("fpsaimforge_nim_visual_{}_{}", stamp, id);
    const auto payload_path = base.string() + ".json";
    const auto response_path = base.string() + ".response";
    const auto config_path = base.string() + ".curl";

    std::error_code ec;
    std::size_t total_image_bytes = 0;
    bool request_too_large = false;
    constexpr std::size_t kMaxVisualImageBytes = 50U * 1024U * 1024U;
    {
      std::ofstream payload(payload_path, std::ios::binary);
      payload << "{\"model\":\"" << JsonEscape(model)
              << "\",\"messages\":[{\"role\":\"system\",\"content\":\""
                 "You are a precise FPS aim coach reviewing a complete chronological replay. "
                 "Use the visual frames together with the measured telemetry. Do not invent "
                 "events that are not visible or supported by the telemetry. Identify exact "
                 "weak points, what happened, where failures occurred, and concrete practice "
                 "actions. The images are chronological and cover the entire scenario.\"},"
                 "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\""
              << JsonEscape(prompt + "\n\nVisual replay: chronological frames sampled at 4 frames per second.")
              << "\"}";

      for (const auto& image_path : image_paths) {
        std::ifstream input(image_path, std::ios::binary);
        if (!input) {
          continue;
        }
        input.seekg(0, std::ios::end);
        const std::streampos size = input.tellg();
        if (size < 0) {
          continue;
        }
        std::string bytes(static_cast<std::size_t>(size), '\0');
        input.seekg(0, std::ios::beg);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input && !input.eof()) {
          continue;
        }
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if (total_image_bytes + bytes.size() > kMaxVisualImageBytes) {
          request_too_large = true;
          break;
        }
        total_image_bytes += bytes.size();
        const std::string encoded = Base64Encode(bytes);
        payload << ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,"
                << encoded << "\",\"detail\":\"low\"}}";
      }

      payload << "]}],\"max_tokens\":1800,\"temperature\":0.2,\"stream\":false}";
    }

    if (request_too_large) {
      std::filesystem::remove(payload_path, ec);
      std::filesystem::remove(config_path, ec);
      std::filesystem::remove(response_path, ec);
      for (const auto& image_path : image_paths) {
        std::filesystem::remove(image_path, ec);
      }
      std::lock_guard lock(state->mutex);
      state->done = true;
      state->error =
          "NVIDIA NIM visual request is too large. Replay frames were capped at 50 MB; "
          "reduce the replay length or captured resolution.";
      return;
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
        "curl --silent --show-error --fail --max-time 180 --config \"" + config_path + "\"";
    const int exit_code = std::system(command.c_str());

    std::string response;
    if (exit_code == 0) {
      response = ReadBinaryFile(response_path);
    }

    std::filesystem::remove(payload_path, ec);
    std::filesystem::remove(config_path, ec);
    std::filesystem::remove(response_path, ec);

    for (const auto& image_path : image_paths) {
      std::filesystem::remove(image_path, ec);
    }

    std::lock_guard lock(state->mutex);
    state->done = true;
    if (exit_code != 0) {
      state->error =
          "NVIDIA NIM visual request failed. Check the API key, vision model, endpoint, "
          "request size, and network connection.";
      return;
    }

    state->response = ExtractJsonString(response, "content");
    state->success = !state->response.empty();
    if (!state->success) {
      const std::string api_error = ExtractJsonString(response, "message");
      const std::string error_detail = ExtractJsonString(response, "detail");
      if (!api_error.empty()) {
        state->error = "NVIDIA NIM rejected the visual request: " + api_error;
        if (!error_detail.empty()) state->error += " (" + error_detail + ")";
      } else {
        state->error = "NVIDIA NIM returned no visual assistant content.";
      }
    }
  }).detach();

  return state;
}


std::shared_ptr<NimAnalysisState> StartNimVideoAnalysis(
    const std::string& prompt, const std::filesystem::path& video_path) {
  auto state = std::make_shared<NimAnalysisState>();

  const std::string api_key =
      !GetEnv("NVIDIA_NIM_API_KEY").empty() ? GetEnv("NVIDIA_NIM_API_KEY")
                                            : GetEnv("NVIDIA_API_KEY");
  const std::string endpoint =
      !GetEnv("NVIDIA_NIM_ENDPOINT").empty() ? GetEnv("NVIDIA_NIM_ENDPOINT")
                                             : kDefaultEndpoint;
  const std::string model = !GetEnv("NVIDIA_NIM_VIDEO_MODEL").empty()
                                ? GetEnv("NVIDIA_NIM_VIDEO_MODEL")
                                : "nvidia/cosmos3-nano-reasoner";

  std::thread([state, prompt, video_path, api_key, endpoint, model]() {
    const auto id = g_request_counter.fetch_add(1);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
                      std::format("fpsaimforge_nim_video_{}_{}", stamp, id);
    const auto payload_path = base.string() + ".json";
    const auto response_path = base.string() + ".response";
    const auto config_path = base.string() + ".curl";
    std::error_code ec;

    const std::string bytes = ReadBinaryFile(video_path);
    constexpr std::size_t kMaxVideoBytes = 45U * 1024U * 1024U;
    if (bytes.empty() || bytes.size() > kMaxVideoBytes) {
      std::lock_guard lock(state->mutex);
      state->done = true;
      state->error = "Replay video could not be read or is larger than 45 MB.";
      return;
    }

    const std::string encoded = Base64Encode(bytes);
    {
      std::ofstream payload(payload_path, std::ios::binary);
      payload << "{\"model\":\"" << JsonEscape(model)
              << "\",\"messages\":[{\"role\":\"user\",\"content\":["
                 "{\"type\":\"text\",\"text\":\""
              << JsonEscape(
                     "You are an FPS aim coach. Watch the entire replay video chronologically. "
                     "Do not invent events. Identify exact weak points, failures, recoveries, "
                     "crosshair behavior, target acquisition and tracking mistakes. " +
                     prompt)
              << "\"},{\"type\":\"video_url\",\"video_url\":{\"url\":\"data:video/mp4;base64,"
              << encoded << "\"}}]}],\"max_tokens\":2200,\"temperature\":0.2,\"stream\":false}";
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
        "curl --silent --show-error --fail --max-time 240 --config \"" +
        config_path + "\"";
    const int exit_code = RunCommandNoWindow(command);

    std::string response;
    if (exit_code == 0) {
      response = ReadBinaryFile(response_path);
    }

    std::filesystem::remove(payload_path, ec);
    std::filesystem::remove(config_path, ec);
    std::filesystem::remove(response_path, ec);
    std::filesystem::remove(video_path, ec);

    std::lock_guard lock(state->mutex);
    state->done = true;
    if (exit_code != 0) {
      state->error =
          "NVIDIA NIM video request failed. Check the video model, API key, endpoint, "
          "request size, and network connection.";
      return;
    }

    state->response = ExtractJsonString(response, "content");
    state->success = !state->response.empty();
    if (!state->success) {
      const std::string api_error = ExtractJsonString(response, "message");
      const std::string detail = ExtractJsonString(response, "detail");
      if (!api_error.empty()) {
        state->error = "NVIDIA NIM rejected the video request: " + api_error;
      } else {
        state->error = "NVIDIA NIM returned no video analysis content.";
      }
      if (!detail.empty()) state->error += " (" + detail + ")";
    }
  }).detach();

  return state;
}


std::shared_ptr<NimAnalysisState> StartNimVideoAnalysisFromFrames(
    const std::string& prompt, const std::vector<std::filesystem::path>& image_paths) {
  auto state = std::make_shared<NimAnalysisState>();
  if (image_paths.empty()) {
    state->done = true;
    state->error = "No replay frames were captured.";
    return state;
  }

  std::thread([state, prompt, image_paths]() {
    const auto id = g_request_counter.fetch_add(1);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
                      std::format("fpsaimforge_replay_video_{}_{}", stamp, id);
    const auto video_path = base.string() + ".mp4";
    const auto log_path = base.string() + ".ffmpeg.log";
    const auto parent = image_paths.front().parent_path();

    std::filesystem::path ffmpeg_path;
#ifdef _WIN32
    if (const char* base_path = SDL_GetBasePath(); base_path != nullptr) {
      ffmpeg_path = std::filesystem::path(base_path) / "ffmpeg.exe";
      SDL_free(const_cast<char*>(base_path));
    }
    if (ffmpeg_path.empty() || !std::filesystem::exists(ffmpeg_path)) {
      ffmpeg_path = "ffmpeg.exe";
    }
#else
    ffmpeg_path = "ffmpeg";
#endif

    const std::string command =
        "\"" + CurlConfigEscape(ffmpeg_path.string()) +
        "\" -hide_banner -loglevel error -y -framerate 4 -i \"" +
        CurlConfigEscape((parent / "frame_%04d.png").string()) +
        "\" -c:v libx264 -preset ultrafast -crf 30 -pix_fmt yuv420p -movflags +faststart \"" +
        CurlConfigEscape(video_path) + "\" > \"" + CurlConfigEscape(log_path) + "\" 2>&1";
    const int exit_code = RunCommandNoWindow(command);
    std::error_code ec;
    std::filesystem::remove(log_path, ec);

    if (exit_code != 0 || !std::filesystem::exists(video_path)) {
      for (const auto& image_path : image_paths) {
        std::filesystem::remove(image_path, ec);
      }
      std::lock_guard lock(state->mutex);
      state->done = true;
      state->error =
          "The bundled FFmpeg encoder could not create the replay video. "
          "The AI request was not sent.";
      return;
    }

    auto video_state = StartNimVideoAnalysis(prompt, video_path);
    while (true) {
      {
        std::lock_guard video_lock(video_state->mutex);
        if (video_state->done) {
          std::lock_guard lock(state->mutex);
          state->done = true;
          state->success = video_state->success;
          state->response = video_state->response;
          state->error = video_state->error;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    for (const auto& image_path : image_paths) {
      std::filesystem::remove(image_path, ec);
    }
  }).detach();

  return state;
}

std::shared_ptr<ReplayExportState> StartReplayExportToMp4(
    const std::vector<std::filesystem::path>& image_paths,
    const std::filesystem::path& output_path,
    int capture_fps) {
  auto state = std::make_shared<ReplayExportState>();
  state->output_path = output_path;

  if (image_paths.empty()) {
    std::lock_guard lock(state->mutex);
    state->done = true;
    state->error = "No frames were captured for export.";
    return state;
  }

  std::thread([state, image_paths, output_path, capture_fps]() {
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);

    const auto parent = image_paths.front().parent_path();
    const auto log_path = output_path.string() + ".ffmpeg.log";

    std::filesystem::path ffmpeg_path;
#ifdef _WIN32
    if (const char* base_path = SDL_GetBasePath(); base_path != nullptr) {
      ffmpeg_path = std::filesystem::path(base_path) / "ffmpeg.exe";
      SDL_free(const_cast<char*>(base_path));
    }
    if (ffmpeg_path.empty() || !std::filesystem::exists(ffmpeg_path)) {
      ffmpeg_path = "ffmpeg.exe";
    }
#else
    ffmpeg_path = "ffmpeg";
#endif

    const std::string command =
        "\"" + CurlConfigEscape(ffmpeg_path.string()) +
        "\" -hide_banner -loglevel error -y -framerate " + std::to_string(capture_fps) +
        " -i \"" + CurlConfigEscape((parent / "frame_%04d.png").string()) +
        "\" -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart \"" +
        CurlConfigEscape(output_path.string()) +
        "\" > \"" + CurlConfigEscape(log_path) + "\" 2>&1";

    const int exit_code = RunCommandNoWindow(command);
    std::filesystem::remove(log_path, ec);

    // Always clean up frame PNGs.
    for (const auto& image_path : image_paths) {
      std::filesystem::remove(image_path, ec);
    }

    std::lock_guard lock(state->mutex);
    state->done = true;
    if (exit_code != 0 || !std::filesystem::exists(output_path)) {
      state->success = false;
      state->error =
          "FFmpeg failed to encode the replay. "
          "Make sure ffmpeg is available and the exports folder is writable.";
    } else {
      state->success = true;
    }
  }).detach();

  return state;
}

}  // namespace aim