// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-GT7-179: ryty-feel-test, the launcher's Test buttons for HD rumble and the adaptive
// triggers. It plays one effect on a DualSense from a short scripted drive, through the same code
// the game uses (telemetry_haptics.h, trigger_feedback.h), so a test feels like the track.
//
//   ryty-feel-test <effect> [setting=value ...]
//
// effect:  engine road slip shift limiter impact   (HD rumble, the controller body; USB only)
//          brake throttle lock spin                (adaptive triggers; USB or Bluetooth)
// setting: the Input::Feel names (haptics_engine=80, trigger_brake=120, ...), 0-200.
//
// Prints "phase: <what is playing>" lines while it runs and "error: <why>" when it cannot; exits
// 0 when done, 1 for bad arguments, 2 when no DualSense (or, for HD rumble, no USB sound card).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "input/dualsense_audio.h"
#include "input/feel_settings.h"
#include "input/telemetry_haptics.h"
#include "input/trigger_feedback.h"

namespace {
namespace Haptics = Input::TelemetryHaptics;
namespace Triggers = Input::TriggerFeedback;
using Clock = std::chrono::steady_clock;

// One moment of the scripted drive.
struct State {
    float speed = 30, rpm = 4000;
    unsigned gear = 3;
    std::uint8_t throttle = 0, brake = 0;
    std::array<float, 4> slip{1, 1, 1, 1}; // wheel speed against car speed
    std::array<float, 4> suspension{0.05f, 0.05f, 0.05f, 0.05f};
    float decel = 0;         // m/s^2, for the brake's firmness
    bool shifting = false;
    const char* phase = "";
};

struct Effect {
    const char* name;
    bool triggers; // a trigger test, else HD rumble
    float seconds;
    void (*script)(float t, unsigned frame, State& state);
};

float Wave(unsigned frame, float amplitude) { // a kerb's stripes: up and down every other frame
    return (frame / 2) % 2 ? amplitude : -amplitude;
}

const Effect Effects[] = {
    {"engine", false, 4.0f, [](float t, unsigned, State& s) {
         const float rise = t < 2.4f ? t / 2.4f : std::max(0.0f, 1 - (t - 2.4f) / 1.4f);
         s.rpm = 1200 + 6700 * rise;
         s.throttle = t < 2.4f ? 255 : 40;
         s.speed = 15 + 20 * rise;
         s.phase = t < 2.4f ? "engine: revving from idle to near the limiter" : "engine: lifting off";
     }},
    {"road", false, 4.6f, [](float t, unsigned frame, State& s) {
         s.speed = 30;
         if (t < 1.0f) {
             for (unsigned w = 0; w < 4; ++w) s.suspension[w] = 0.05f + (((frame * 7 + w * 3) % 5) - 2) * 0.0012f;
             s.phase = "road: rough tarmac";
         } else if (t >= 1.2f && t < 2.2f) {
             s.suspension[0] = s.suspension[2] = 0.05f + Wave(frame, 0.006f);
             s.phase = "road: a kerb under the left wheels";
         } else if (t >= 2.6f && t < 3.6f) {
             s.suspension[1] = s.suspension[3] = 0.05f + Wave(frame, 0.006f);
             s.phase = "road: a kerb under the right wheels";
         } else if (t >= 3.9f && t < 3.93f) {
             s.suspension.fill(0.065f);
             s.phase = "road: a bump under both sides";
         } else {
             s.phase = t < 3.9f ? "road: smooth" : "road: a bump under both sides";
         }
     }},
    {"slip", false, 4.0f, [](float t, unsigned, State& s) {
         s.speed = 25;
         if (t >= 0.3f && t < 1.6f) {
             s.slip[3] = 1.6f;
             s.throttle = 255;
             s.phase = "slip: wheelspin, right rear";
         } else if (t >= 2.2f && t < 3.5f) {
             s.slip[0] = 0.45f;
             s.brake = 255;
             s.phase = "slip: wheel lock, front left";
         } else {
             s.phase = "slip: rolling";
         }
     }},
    {"shift", false, 3.8f, [](float t, unsigned, State& s) {
         s.gear = 2 + std::min(4u, static_cast<unsigned>(t / 0.7f));
         s.rpm = 5000;
         s.throttle = 200;
         s.phase = "shift: gear changes up through the box";
     }},
    {"limiter", false, 3.0f, [](float, unsigned, State& s) {
         s.rpm = 8000;
         s.throttle = 255;
         s.speed = 20;
         s.phase = "limiter: holding the engine on the rev limiter";
     }},
    {"impact", false, 4.0f, [](float t, unsigned, State& s) {
         // The car loses 3 m/s in one frame (a hard crash), later 1 m/s more (a lighter hit).
         s.speed = t < 1.0f ? 35.0f : t < 2.6f ? 32.0f : 31.0f;
         s.phase = t < 1.0f ? "impact: driving" : t < 2.6f ? "impact: a hard crash" : "impact: a lighter hit";
     }},
    {"brake", true, 7.5f, [](float t, unsigned, State& s) {
         s.speed = t < 2.5f ? 0 : 30;
         s.decel = t >= 5.0f ? 10 : 0;
         s.phase = t < 2.5f ? "brake: pull L2 - standing still"
                 : t < 5.0f ? "brake: pull L2 - cruising at 110 km/h"
                            : "brake: pull L2 - braking hard";
     }},
    {"throttle", true, 7.5f, [](float t, unsigned frame, State& s) {
         s.speed = 20;
         if (t < 2.5f) {
             s.rpm = 2000;
             s.phase = "throttle: pull R2 - low revs";
         } else if (t < 5.0f) {
             s.rpm = 7000;
             s.shifting = (frame % 48) < 6; // a gear change every 0.8 s
             s.phase = "throttle: pull R2 - high revs, with gear changes";
         } else {
             s.rpm = 8000;
             s.throttle = 255;
             s.phase = "throttle: pull R2 - on the rev limiter";
         }
     }},
    {"lock", true, 4.0f, [](float t, unsigned, State& s) {
         s.brake = 255;
         // ABS-like: the wheel locks and frees a few times a second.
         s.slip[0] = static_cast<unsigned>(t / 0.25f) % 2 ? 1.0f : 0.45f;
         s.phase = "lock: pull L2 - the front-left wheel locking under braking";
     }},
    {"spin", true, 4.0f, [](float t, unsigned, State& s) {
         s.throttle = 255;
         s.slip[2] = s.slip[3] = t < 2.0f ? 1.6f : 1.25f;
         s.phase = t < 2.0f ? "spin: pull R2 - the rear wheels spinning hard"
                            : "spin: pull R2 - the rear wheels slipping";
     }},
};

// The audio callback's state.
std::mutex synth_mutex;
Haptics::Synth synth;
Input::DualSenseAudio::Device audio;
std::vector<float> buffer;

void SDLCALL Feed(void*, SDL_AudioStream* stream, int additional, int) {
    if (additional <= 0 || audio.channels <= 0) return;
    const int frames = additional / static_cast<int>(sizeof(float) * audio.channels);
    if (frames <= 0) return;
    buffer.assign(static_cast<std::size_t>(frames) * audio.channels, 0.0f);
    {
        std::lock_guard lock(synth_mutex);
        synth.Render(buffer.data(), frames, audio.channels, audio.left, audio.right);
    }
    SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(buffer.size() * sizeof(float)));
}

SDL_Gamepad* OpenDualSense() {
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    SDL_Gamepad* found = nullptr;
    for (int i = 0; pads && i < count && !found; ++i) {
        if (SDL_GetGamepadTypeForID(pads[i]) == SDL_GAMEPAD_TYPE_PS5) found = SDL_OpenGamepad(pads[i]);
    }
    SDL_free(pads);
    return found;
}

void SendTriggers(SDL_Gamepad* pad, const Triggers::Effect& left, const Triggers::Effect& right) {
    const auto packet = Triggers::DualSensePacket(left, right);
    SDL_SendGamepadEffect(pad, packet.data(), static_cast<int>(packet.size()));
}

void Say(const char* kind, const char* text) {
    std::printf("%s: %s\n", kind, text);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Say("error", "usage: ryty-feel-test <effect> [setting=value ...]");
        return 1;
    }
    const Effect* effect = nullptr;
    for (const auto& candidate : Effects) {
        if (std::strcmp(argv[1], candidate.name) == 0) effect = &candidate;
    }
    if (!effect) {
        Say("error", (std::string("unknown effect '") + argv[1] + "'").c_str());
        return 1;
    }
    Input::Feel::Settings settings;
    for (int i = 2; i < argc; ++i) {
        if (!Input::Feel::ParseArgument(argv[i], settings)) {
            Say("error", (std::string("bad setting '") + argv[i] + "'").c_str());
            return 1;
        }
    }
    // A line (or the end) on stdin stops the test early, still releasing the triggers: the launcher
    // sends one when another Test starts or its window closes.
    static std::atomic<bool> quit{false};
    std::thread([] {
        int c;
        while ((c = std::getchar()) != EOF && c != '\n') {
        }
        quit = true;
    }).detach();
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        Say("error", SDL_GetError());
        return 2;
    }
    SDL_Gamepad* pad = nullptr;
    if (effect->triggers) {
        pad = OpenDualSense();
        if (!pad) {
            Say("error", "no DualSense found; connect it by USB or Bluetooth");
            SDL_Quit();
            return 2;
        }
    } else {
        std::string error;
        if (!Input::DualSenseAudio::Open(audio, &Feed, nullptr, error)) {
            Say("error", error.empty() ? "no DualSense sound card; HD rumble needs the controller connected by USB cable"
                                       : error.c_str());
            SDL_Quit();
            return 2;
        }
        synth.SetStrength(settings.haptics / 100.0f);
        SDL_ResumeAudioStreamDevice(audio.stream);
    }

    Haptics::Gains gains;
    gains.engine = settings.engine / 100.0f;
    gains.road = settings.road / 100.0f;
    gains.slip = settings.slip / 100.0f;
    gains.shift = settings.shift / 100.0f;
    gains.limiter = settings.limiter / 100.0f;
    gains.impact = settings.impact / 100.0f;
    // A haptics test plays only its own effect, so the engine hum does not cover a kerb.
    if (!effect->triggers) {
        const Haptics::Gains all = gains;
        gains = {};
        gains.engine = gains.road = gains.slip = gains.shift = gains.limiter = gains.impact = 0;
        const std::string_view name = effect->name;
        if (name == "engine") gains.engine = all.engine;
        if (name == "road") gains.road = all.road;
        if (name == "slip") gains.slip = all.slip;
        if (name == "shift") gains.shift = all.shift;
        if (name == "limiter") gains.limiter = all.limiter;
        if (name == "impact") gains.impact = all.impact;
    }

    Haptics::Analyzer analyzer;
    std::string phase;
    const auto start = Clock::now();
    const auto step = std::chrono::microseconds(16667);
    Triggers::Effect sent_left{}, sent_right{};
    for (unsigned frame = 0;; ++frame) {
        const float t = static_cast<float>(frame) / 60.0f;
        if (t >= effect->seconds || quit) break;
        State state;
        effect->script(t, frame, state);
        if (state.phase != phase) {
            phase = state.phase;
            Say("phase", phase.c_str());
        }
        if (effect->triggers) {
            Triggers::Sample sample;
            sample.packet = frame + 1;
            sample.speed = state.speed;
            sample.flags = 1;
            sample.throttle = state.throttle;
            sample.brake = state.brake;
            sample.rpm = state.rpm;
            sample.rpm_limit = 8000;
            sample.gear = static_cast<std::uint8_t>(state.gear);
            for (unsigned w = 0; w < 4; ++w) {
                sample.radius[w] = 0.3f;
                // Calculate() needs a moving car to see lock and spin.
                sample.omega[w] = std::max(state.speed, 25.0f) * state.slip[w] / 0.3f;
            }
            if (sample.speed < 3 && (state.brake || state.throttle)) sample.speed = 25;
            const auto output = Triggers::Compute(sample, state.decel, state.shifting, Clock::now(), settings);
            if (output.left != sent_left || output.right != sent_right) {
                SendTriggers(pad, output.left, output.right);
                sent_left = output.left;
                sent_right = output.right;
            }
            SDL_UpdateGamepads();
        } else {
            Haptics::Frame f;
            f.packet = frame + 1;
            f.velocity = {state.speed, 0, 0};
            f.rpm = state.rpm;
            f.speed = state.speed;
            f.rpm_limit = 8000;
            f.flags = 1;
            f.gear = static_cast<std::uint8_t>(state.gear);
            f.throttle = state.throttle;
            f.brake = state.brake;
            for (unsigned w = 0; w < 4; ++w) {
                f.radius[w] = 0.3f;
                f.omega[w] = state.speed * state.slip[w] / 0.3f;
                f.suspension[w] = state.suspension[w];
            }
            const auto events = analyzer.Update(f, 1.0f / 60.0f, gains);
            std::lock_guard lock(synth_mutex);
            synth.Apply(events);
        }
        std::this_thread::sleep_until(start + step * (frame + 1));
    }

    if (pad) {
        SendTriggers(pad, Triggers::Vibration(0), Triggers::Vibration(0));
        SDL_Delay(50);
        SDL_CloseGamepad(pad);
    }
    if (audio.stream) {
        {
            std::lock_guard lock(synth_mutex);
            synth.Stop();
        }
        SDL_Delay(400); // let the last effect fade
        Input::DualSenseAudio::Close(audio);
    }
    Say("phase", "done");
    SDL_Quit();
    return 0;
}
