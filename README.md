## FspAimForge

FpsAimForge is an open source aim trainer focused on simplicity and self improvement.

* Play on [Steam](https://store.steampowered.com/app/5165380/FpsAimForge/)
* [All releases](https://github.com/mjohns/FpsAimForge/releases)
* [Reddit](https://www.reddit.com/r/FpsAimForge/)

Major goals:
* Creating and editing scenarios is actually simple.
* Easy to play scenarios at varying difficulty levels and sensitivities while tracking/comparing progress. (progressive overload)
* Amazing performance due to custom "engine" and local first (offline) design.
* Small app download while still including playlists annd scenarios. You can download the full exe and a large selection of scenarios in just a 5mb download. No additional runtime downloads required.

See [videos](https://www.youtube.com/@FpsAimForge) of FpsAimForge in action!

Prebuilt binaries can be found under the [releases](https://github.com/mjohns/FpsAimForge/releases) tab.

# Features
* Settings like sensitivity, theme, and crosshair are saved per scenario (optional)
* Quickly change settings and sensitivity. Hold "s" to bring up the quick settings screen and release "s" to save. Use the scroll wheel while holding "s" to change senstivity. [video](https://www.youtube.com/watch?v=PHVEZ-ijGzM)
* Adjust crosshair size by holding "c" and using the scroll wheel.
* View replays of your last run from the stats screen.
* Simplified and consistent scoring. Tracking scenarios are score directly on hit percent which is calculated at the microsecond level and with granularity based on update/second.
* Every scenario automatically has x%Smaller, x%Faster type versions available. Version selector dialog helps construct these dynamically generated scenarios.

# Interesting scenario types
* Proximity tracking. Tracking variation where score is based on how close to the center you are.
* Option to remove closest target on miss.
* Switching scenarios where the target radius shrinks as health is taken away.
* Switching scenarios not as focused on health timing allowing you to respond to the kill sound to move to the next target. (Sound played when x% health left, target removed once you move off after sound, You get score based on percent of health taken from target).

# Overview
Scenarios and playlists are distributed in "bundles" which are namespaced collections. So the AF bundle contains scenarios and playlists that all start with AF, like "AF Clicking", "AF Reflex Click", etc.
This allows sharing these bundles without naming collisions. The files are placed within the bundles folder in the user folder which can be opened from the settings page.

# Technical Overview
FpsAimForge is a c++ app built on top of SDL3 (and the GPU api) and ImGui. Stats and settings are tracked using sqlite3 and scenarios and other config files are represented using json serialized protobufs. The app uses a simple custom "engine" that can efficiently render scenarios. It is currently fairly well optimized and can run at 5000 fps or capped at 1200 fps with 500k state updates per second (event polling, hit detection, etc). The code is also simple and focused enough that further optimizations should be straightforward to implement (compared to using something like Unreal). Effort was also put into making sure the worst frame is still good and can be easily viewed after a run in the perf tab. Typically the worst frame has a latency which projects 1300 fps.

# Building
The project is built with CMake and Ninja.
To build on windows you can install Visual Studio (not Visual Studio Code) and select "Desktop development in c++" during
installation. This will automatically install versions for CMake and Ninja. You can open the folder in Visual Studio and build the FpsAimForge.exe target.

On Unix like systems you will need to make sure CMake and [Ninja](https://ninja-build.org/) are installed (and a c++ compiler).
Then run the following commands:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target FpsAimForge && ./build/bin/FpsAimForge
```

To build protoc run:
```bash
cmake --build build --target protoc
ls build/bin/protoc
```

Use test.sh to run tests. It optionally takes a filter. Or run the following commands:
```bash
cmake --build build --target FpsAimForgeTests
cd build
ctest
```


## Scenario analysis and AI coaching

Completed runs can now be opened from the stats screen with **Detailed Analysis**. The analysis is built from the existing replay capture and reports accuracy, miss streaks, click pacing, mouse movement speed, direction reversals, and target-tracking error when target snapshots are available.

- **Detailed Analysis**: deterministic replay-derived diagnostics.
- **Weak Points**: concrete findings with replay timestamps when available.
- **Replay Review**: opens the existing replay viewer for the same run.
- **Watch replay**: jumps directly into replay playback.
- **AI Overview**: optional NVIDIA NIM coaching based on the measured analysis.

To enable NVIDIA NIM hosted analysis, set \`NVIDIA_NIM_API_KEY\`. Optional environment variables are \`NVIDIA_NIM_MODEL\` and \`NVIDIA_NIM_ENDPOINT\`. The default endpoint is NVIDIA's OpenAI-compatible \`/v1/chat/completions\` endpoint and the default model is \`deepseek-ai/deepseek-v4-flash\`.

The NIM request runs asynchronously so the replay/stats UI remains responsive. If NIM is not configured or unavailable, deterministic analysis remains fully local and usable.

### Release build

GitHub Actions now builds the release target, runs the existing test suite, and uploads a \`FpsAimForge-release\` artifact on pushes to \`main\` or manual workflow dispatch.


### Full visual replay AI review

The AI reviewer can now render the complete recorded scenario at up to 60 visual checkpoints, capture the replay visuals as temporary PNG frames, and send the chronological frame sequence plus deterministic replay telemetry to NVIDIA NIM. Visual capture happens only when the user starts **Watch Full Scenario with AI**, not during normal gameplay. Temporary frames are deleted after the NIM request completes.

The default visual model is `deepseek-ai/deepseek-v4.1-flash`, which supports image input. Override it with `NVIDIA_NIM_VISION_MODEL` when needed.


## AI replay coaching

Replay analysis is available from the Stats screen. The deterministic analyzer reads recorded replay telemetry for accuracy, click pacing, movement speed, direction changes, target-relative tracking error, tracking losses, recovery time, and timestamped failure points.

**Full visual review** captures the complete replay chronologically at up to 60 visual checkpoints. Frames are downsampled to a maximum of 480x270 RGB PNG before being sent to the configured NVIDIA NIM vision model. Capture does not alter scenario logic, hit registration, score calculation, target movement, or the legacy scenario bundle.

Set `NVIDIA_NIM_API_KEY` or `NVIDIA_API_KEY` for NVIDIA-hosted inference. `NVIDIA_NIM_MODEL`, `NVIDIA_NIM_VISION_MODEL`, and `NVIDIA_NIM_ENDPOINT` can override the model or endpoint. The default visual model is `deepseek-ai/deepseek-v4.1-flash`.

The Windows release package contains `FpsAimForge.exe`, `resources/`, and compiled `shaders/`. The release workflow builds Linux and Windows x64 artifacts, runs the tests, verifies gameplay-critical source parity against upstream, and uses the static MSVC runtime for the Windows package.
