// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-GT7-179: DualSense haptics from the game's Simulator Interface telemetry. The controller's
// own motor rumble (the game's two motors) keeps playing; these are short events layered on top
// through the controller's voice-coil actuators, on the side of the car they happen on: kerbs and
// bumps under the left or right wheels, wheel lock and wheelspin per side, gear changes, the rev
// limiter and impacts.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Input::TelemetryHaptics {

// The fields used, from GT Sport v1.69's (and GT7's) little-endian 0x128-byte packet.
struct Frame {
    std::uint32_t packet{};
    std::array<float, 3> velocity{};      // 0x10, m/s, world space
    float rpm{};                          // 0x3c
    float speed{};                        // 0x4c, m/s
    std::uint16_t rpm_limit{};            // 0x8a, the rev-limit warning's upper end
    std::uint16_t flags{};                // 0x8e
    std::uint8_t gear{};                  // 0x90, low nibble
    std::uint8_t throttle{}, brake{};     // 0x91, 0x92
    std::array<float, 4> omega{}, radius{}, suspension{}; // 0xa4, 0xb4, 0xc4; FL FR RL RR
};

template <class T> inline T Read(const std::uint8_t* data, unsigned offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

inline bool Decode(const std::uint8_t* data, Frame& frame) {
    if (Read<std::uint32_t>(data, 0) != 0x47375330) return false;
    frame.packet = Read<std::uint32_t>(data, 0x70);
    for (unsigned i = 0; i < 3; ++i) frame.velocity[i] = Read<float>(data, 0x10 + 4 * i);
    frame.rpm = Read<float>(data, 0x3c);
    frame.speed = Read<float>(data, 0x4c);
    frame.rpm_limit = Read<std::uint16_t>(data, 0x8a);
    frame.flags = Read<std::uint16_t>(data, 0x8e);
    frame.gear = data[0x90] & 0x0f;
    frame.throttle = data[0x91];
    frame.brake = data[0x92];
    if (!std::isfinite(frame.speed) || frame.speed < 0 || frame.speed > 200) return false;
    if (!std::isfinite(frame.rpm) || frame.rpm < 0 || frame.rpm > 30000) return false;
    for (float v : frame.velocity) if (!std::isfinite(v) || std::abs(v) > 250) return false;
    for (unsigned i = 0; i < 4; ++i) {
        frame.omega[i] = Read<float>(data, 0xa4 + 4 * i);
        frame.radius[i] = Read<float>(data, 0xb4 + 4 * i);
        frame.suspension[i] = Read<float>(data, 0xc4 + 4 * i);
        if (!std::isfinite(frame.omega[i]) || std::abs(frame.omega[i]) > 20000 ||
            !std::isfinite(frame.radius[i]) || frame.radius[i] < 0.05f || frame.radius[i] > 2 ||
            !std::isfinite(frame.suspension[i]) || std::abs(frame.suspension[i]) > 2)
            return false;
    }
    return true;
}

// On track (flags bit 0), not paused or loading (bits 1, 2).
inline bool Driving(std::uint16_t flags) {
    return (flags & 1) && !(flags & 6);
}

// What a telemetry frame asks of each side: knocks start or strengthen a short thump, slip sets a
// gritty texture for as long as it lasts. 0 = left (FL, RL), 1 = right (FR, RR).
struct Events {
    std::array<float, 2> knock{}, slip{};
    std::array<float, 2> knock_hz{50.0f, 50.0f}; // pitch of the strongest knock on each side
    std::array<float, 2> knock_noise{};          // 0 = clean thud, 1 = a rough crash
    float engine{};                              // engine hum level, 0-1
    float engine_hz{};                           // its pitch, following the RPM
};

// Tuning, in the packet's units. Suspension heights are metres; at 60 frames a second a kerb or a
// sharp bump changes a wheel's suspension velocity by a few centimetres a frame.
struct Tuning {
    float bump_floor = 0.0015f;   // second difference (m) below this is ordinary road
    float bump_range = 0.0120f;   // ... and this much above the floor is a full knock
    float slip_floor = 0.08f;     // wheel speed against car speed, as the adaptive triggers use
    float slip_range = 0.25f;
    float impact_floor = 45.0f;   // m/s^2 (about 4.5 g); cornering reaches 3-4 g in GT Sport
    float impact_range = 60.0f;
    float impact_ignore = 600.0f; // larger changes are a reset or teleport, not a crash
};

// How strong each kind of effect is: 1 standard, 0 off (the launcher's sliders / 100).
struct Gains {
    float engine = 1, road = 1, slip = 1, shift = 1, limiter = 1, impact = 1;
};

class Analyzer {
public:
    // Turns one telemetry frame into events. dt is the time since the previous frame in seconds.
    Events Update(const Frame& frame, float dt, const Gains& gains = {}, const Tuning& tuning = {}) {
        Events events;
        const bool driving = Driving(frame.flags) && dt > 0 && dt < 0.25f;
        if (!driving || !have_previous) {
            Remember(frame);
            limiter_frames = 0;
            return events;
        }
        // Bumps and kerbs: how sharply each wheel's suspension changed direction.
        for (unsigned wheel = 0; wheel < 4; ++wheel) {
            const float second = std::abs(frame.suspension[wheel] - 2 * previous.suspension[wheel] +
                                          before_previous[wheel]);
            const float level = std::clamp((second - tuning.bump_floor) / tuning.bump_range, 0.0f, 1.0f);
            const unsigned side = wheel & 1;
            Knock(events, side, level * gains.road, 50.0f, 0.3f);
            last_bump[wheel] = second;
        }
        // Wheel lock and wheelspin, per side.
        if (frame.speed > 3) {
            for (unsigned wheel = 0; wheel < 4; ++wheel) {
                const float ratio = std::abs(frame.omega[wheel]) * frame.radius[wheel] / frame.speed;
                const float slip = std::max(1 - ratio, ratio - 1);
                const float level = std::clamp((slip - tuning.slip_floor) / tuning.slip_range, 0.0f, 1.0f);
                events.slip[wheel & 1] = std::max(events.slip[wheel & 1], level * gains.slip);
            }
        }
        // Gear changes: one knock on both sides.
        if (frame.gear != previous.gear && frame.gear != 0 && previous.gear != 0 && frame.speed > 1) {
            for (unsigned side = 0; side < 2; ++side) Knock(events, side, 0.55f * gains.shift, 90.0f, 0.0f);
        }
        // Rev limiter: knocks about 12 times a second while on the limiter with the throttle down.
        if (frame.rpm_limit > 1000 && frame.rpm >= frame.rpm_limit && frame.throttle > 128) {
            if (limiter_frames++ % 5 == 0) for (unsigned side = 0; side < 2; ++side) Knock(events, side, 0.35f * gains.limiter, 70.0f, 0.1f);
        } else {
            limiter_frames = 0;
        }
        // Impacts: a sudden change in the car's velocity.
        float change = 0;
        for (unsigned i = 0; i < 3; ++i) change += (frame.velocity[i] - previous.velocity[i]) * (frame.velocity[i] - previous.velocity[i]);
        last_acceleration = std::sqrt(change) / dt;
        if (last_acceleration > tuning.impact_floor && last_acceleration < tuning.impact_ignore) {
            const float level = std::clamp(0.4f + (last_acceleration - tuning.impact_floor) / tuning.impact_range, 0.0f, 1.0f);
            for (unsigned side = 0; side < 2; ++side) Knock(events, side, level * gains.impact, 38.0f, 1.0f);
        }
        // Engine: a faint hum from about 35 Hz at idle to about 140 Hz at the limiter, louder with
        // the throttle.
        if (frame.rpm_limit > 1000 && frame.rpm > 0) {
            const float range = std::clamp(frame.rpm / frame.rpm_limit, 0.0f, 1.1f);
            events.engine_hz = 35.0f + 105.0f * range;
            events.engine = (0.10f + 0.20f * (frame.throttle / 255.0f)) * gains.engine;
        }
        Remember(frame);
        return events;
    }
    void Reset() { have_previous = false; limiter_frames = 0; }
    static void Knock(Events& events, unsigned side, float level, float hz, float noise) {
        if (level <= 0 || level <= events.knock[side]) return;
        events.knock[side] = level;
        events.knock_hz[side] = hz;
        events.knock_noise[side] = noise;
    }
    // For the diagnosis log.
    std::array<float, 4> last_bump{};
    float last_acceleration{};

private:
    void Remember(const Frame& frame) {
        before_previous = have_previous ? previous.suspension : frame.suspension;
        previous = frame;
        have_previous = true;
    }
    Frame previous{};
    std::array<float, 4> before_previous{};
    bool have_previous{};
    unsigned limiter_frames{};
};

// Renders the events as the two actuator channels at 48 kHz. A knock is a short, quickly decaying
// thump (a low tone with some noise, felt as a hit rather than a buzz); slip is a gritty texture of
// small random ticks.
class Synth {
public:
    static constexpr int SampleRate = 48000;
    explicit Synth(float strength = 1.0f) : strength(strength) {}

    void Apply(const Events& events) {
        for (unsigned side = 0; side < 2; ++side) {
            if (events.knock[side] > knock[side]) {
                knock[side] = events.knock[side];
                knock_hz[side] = events.knock_hz[side];
                knock_noise[side] = events.knock_noise[side];
            }
            slip_target[side] = events.slip[side];
        }
        engine_target = events.engine;
        if (events.engine_hz > 0) engine_hz_target = events.engine_hz;
    }
    void SetStrength(float value) { strength = value; }
    // Silences everything (menus, pause, stale telemetry).
    void Stop() {
        knock = {};
        slip_target = {};
        engine_target = 0;
    }
    void Render(float* out, int frames, int stride, int left, int right) {
        static const float knock_decay = std::exp(-1.0f / (0.035f * SampleRate));
        static const float tick_decay = std::exp(-1.0f / (0.004f * SampleRate));
        static const float slip_follow = 1.0f - std::exp(-1.0f / (0.030f * SampleRate));
        // The engine's level and pitch glide over about 50 ms, so 60 Hz telemetry does not step.
        static const float engine_follow = 1.0f - std::exp(-1.0f / (0.050f * SampleRate));
        constexpr float TwoPi = 6.2831853f;
        for (int frame = 0; frame < frames; ++frame) {
            engine += (engine_target - engine) * engine_follow;
            engine_hz += (engine_hz_target - engine_hz) * engine_follow;
            engine_phase += TwoPi * engine_hz / SampleRate;
            if (engine_phase > TwoPi) engine_phase -= TwoPi;
            // A pulsing hum (each firing a soft push) rather than a pure, buzzy sine.
            const float firing = std::sin(engine_phase);
            const float hum = engine * firing * std::abs(firing);
            float* sample = out + static_cast<std::size_t>(frame) * stride;
            for (unsigned side = 0; side < 2; ++side) {
                knock[side] *= knock_decay;
                knock_phase[side] += TwoPi * knock_hz[side] / SampleRate;
                if (knock_phase[side] > TwoPi) knock_phase[side] -= TwoPi;
                slip[side] += (slip_target[side] - slip[side]) * slip_follow;
                // Slip: random ticks, more often and stronger with more slip.
                if (slip[side] > 0.01f && Random() < slip[side] * 0.006f) tick[side] = 0.4f + 0.6f * slip[side];
                tick[side] *= tick_decay;
                low[side] += 0.08f * (Noise() - low[side]); // low-passed noise for a rough knock
                const float body = (1 - 0.5f * knock_noise[side]) * std::sin(knock_phase[side]) +
                                   (0.3f + 0.9f * knock_noise[side]) * low[side];
                const float value = knock[side] * body + tick[side] * Noise() + hum;
                sample[side == 0 ? left : right] = std::clamp(value * 0.9f * strength, -1.0f, 1.0f);
            }
        }
    }

private:
    float Random() { // 0..1
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    }
    float Noise() { return Random() * 2 - 1; }
    float strength;
    float engine{}, engine_target{}, engine_hz = 35.0f, engine_hz_target = 35.0f, engine_phase{};
    std::uint32_t seed = 0x12345678u;
    std::array<float, 2> knock{}, knock_hz{50.0f, 50.0f}, knock_noise{}, knock_phase{};
    std::array<float, 2> slip{}, slip_target{}, tick{}, low{};
};

} // namespace Input::TelemetryHaptics
