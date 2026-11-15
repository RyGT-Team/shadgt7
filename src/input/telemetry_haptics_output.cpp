// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-GT7-179: telemetry haptics on a USB DualSense. The effects play on the controller's
// actuators through its sound card, on top of the controller's own motor rumble (the controller
// mixes the two), each scaled by the launcher's HD rumble settings (Input::Feel), which can be
// changed while playing. Over Bluetooth there is no sound card and nothing plays.
#include "input/telemetry_haptics_output.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "common/logging/log.h"
#include "input/dualsense_audio.h"
#include "input/feel_settings.h"
#include "input/telemetry_haptics.h"

namespace Input::TelemetryHaptics {
namespace {
using Clock = std::chrono::steady_clock;

struct State {
    std::mutex mutex;
    Analyzer analyzer;
    Synth synth;
    Clock::time_point last{};
    std::uint32_t last_packet{};
    bool have_last{};
    // Written by the device thread while the stream is paused or closed.
    DualSenseAudio::Device device;
    std::vector<float> buffer;
};
State& Get() {
    static State state;
    return state;
}
std::once_flag started;

Gains GainsFrom(const Feel::Settings& settings) {
    Gains gains;
    gains.engine = settings.engine / 100.0f;
    gains.road = settings.road / 100.0f;
    gains.slip = settings.slip / 100.0f;
    gains.shift = settings.shift / 100.0f;
    gains.limiter = settings.limiter / 100.0f;
    gains.impact = settings.impact / 100.0f;
    return gains;
}

void SDLCALL Feed(void*, SDL_AudioStream* stream, int additional, int) {
    auto& state = Get();
    const int channels = state.device.channels;
    if (additional <= 0 || channels <= 0) return;
    const int frames = additional / static_cast<int>(sizeof(float) * channels);
    if (frames <= 0) return;
    state.buffer.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
    {
        std::lock_guard lock(state.mutex);
        // No telemetry for a quarter second (menus, loading, the game paused): fade out.
        if (!state.have_last || Clock::now() - state.last > std::chrono::milliseconds(250)) {
            state.synth.Stop();
        }
        state.synth.Render(state.buffer.data(), frames, channels, state.device.left, state.device.right);
    }
    SDL_PutAudioStreamData(stream, state.buffer.data(),
                           static_cast<int>(state.buffer.size() * sizeof(float)));
}

void Run() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR(Input, "Telemetry haptics: SDL audio init failed: {}", SDL_GetError());
        return;
    }
    auto& state = Get();
    bool reported = false;
    while (true) {
        if (!state.device.stream) {
            std::string error;
            if (DualSenseAudio::Open(state.device, &Feed, nullptr, error)) {
                SDL_ResumeAudioStreamDevice(state.device.stream);
                LOG_INFO(Input, "Telemetry haptics (FEAT-GT7-179) play on '{}' ({} channels)",
                         state.device.name, state.device.channels);
                std::fprintf(stderr, "[haptics] FEAT-GT7-179 telemetry haptics play on '%s'\n",
                             state.device.name.c_str());
                reported = false;
            } else if (!reported) {
                reported = true;
                if (!error.empty()) LOG_ERROR(Input, "Telemetry haptics: {}", error);
                std::fprintf(stderr, "[haptics] no DualSense sound card (USB) yet; telemetry "
                                     "haptics start when one appears\n");
            }
        } else if (!DualSenseAudio::Present(state.device.id)) {
            DualSenseAudio::Close(state.device);
            std::fprintf(stderr, "[haptics] the DualSense sound card went away\n");
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
} // namespace

void Publish(const std::uint8_t* packet) {
    Frame frame;
    if (!Decode(packet, frame)) return;
    auto& state = Get();
    std::call_once(started, [] { std::thread(Run).detach(); });
    // (diagnosis) RYGT_HAPTICS_LOG=1: what the effects see, twice a second while driving.
    static const bool log = [] {
        const char* env = std::getenv("RYGT_HAPTICS_LOG");
        return env && env[0] == '1';
    }();
    const auto settings = Feel::Get();
    std::lock_guard lock(state.mutex);
    const auto now = Clock::now();
    if (state.have_last && frame.packet == state.last_packet) return;
    const float dt = state.have_last ? std::chrono::duration<float>(now - state.last).count() : 0.0f;
    state.last = now;
    state.last_packet = frame.packet;
    state.have_last = true;
    const Events events = state.analyzer.Update(frame, dt, GainsFrom(settings));
    state.synth.SetStrength(settings.haptics / 100.0f);
    state.synth.Apply(events);
    static unsigned frames = 0;
    if (log && Driving(frame.flags) && ++frames % 30 == 0) {
        const auto& bump = state.analyzer.last_bump;
        std::fprintf(stderr,
                     "[haptics] dt %.4f speed %.1f rpm %.0f/%u gear %u thr %u | bump FL %.4f FR %.4f "
                     "RL %.4f RR %.4f | accel %.1f | knock %.2f/%.2f slip %.2f/%.2f engine %.2f @ %.0f Hz\n",
                     dt, frame.speed, frame.rpm, frame.rpm_limit, frame.gear, frame.throttle, bump[0],
                     bump[1], bump[2], bump[3], state.analyzer.last_acceleration, events.knock[0],
                     events.knock[1], events.slip[0], events.slip[1], events.engine,
                     events.engine_hz);
    }
}
} // namespace Input::TelemetryHaptics
