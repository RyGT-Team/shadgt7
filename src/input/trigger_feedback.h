// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "input/feel_settings.h"

namespace Input::TriggerFeedback {
using Clock = std::chrono::steady_clock;
struct Sample {
    std::uint32_t packet{};
    float speed{};
    std::array<float, 4> omega{}, radius{};
    std::uint16_t flags{};
    std::uint8_t throttle{}, brake{};
    float rpm{};                // FEAT-GT7-179: for the accelerator's resistance
    std::uint16_t rpm_limit{};  // the rev-limit warning's upper end
    std::uint8_t gear{};
};
template<class T> inline T Read(const std::uint8_t* data, unsigned offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}
// GT Sport v1.69's little-endian 0x128-byte Simulator Interface packet.
inline bool Decode(const std::uint8_t* data, Sample& sample) {
    if (Read<std::uint32_t>(data, 0) != 0x47375330) return false;
    sample.packet = Read<std::uint32_t>(data, 0x70);
    sample.speed = Read<float>(data, 0x4c);
    sample.flags = Read<std::uint16_t>(data, 0x8e);
    sample.throttle = data[0x91];
    sample.brake = data[0x92];
    sample.rpm = Read<float>(data, 0x3c);
    sample.rpm_limit = Read<std::uint16_t>(data, 0x8a);
    sample.gear = data[0x90] & 0x0f;
    if (!std::isfinite(sample.rpm) || sample.rpm < 0 || sample.rpm > 30000) sample.rpm = 0;
    if (!std::isfinite(sample.speed) || sample.speed < 0 || sample.speed > 200) return false;
    for (unsigned i = 0; i < 4; ++i) {
        sample.omega[i] = Read<float>(data, 0xa4 + 4*i);
        sample.radius[i] = Read<float>(data, 0xb4 + 4*i);
        if (!std::isfinite(sample.omega[i]) || std::abs(sample.omega[i]) > 20000 ||
            !std::isfinite(sample.radius[i]) || sample.radius[i] < 0.05f || sample.radius[i] > 2)
            return false;
    }
    return true;
}
struct Feedback {
    std::uint8_t left{}, right{};
};
// Telemetry from the car being driven: on track (flags bit 0), not paused or loading (bits 1, 2).
// (diagnosis) RYGT_ADAPTIVE_TRIGGERS_REPLAY=1 also takes replays and demos (bit 0 clear), to check
// the effects without driving.
inline bool Driving(std::uint16_t flags) {
    static const bool replays = [] {
        const char* env = std::getenv("RYGT_ADAPTIVE_TRIGGERS_REPLAY");
        return env && env[0] == '1';
    }();
    return ((flags & 1) || replays) && !(flags & 6);
}
inline Feedback Calculate(const Sample& sample) {
    // Never actuate in menus, pause/loading, or at near-zero speed.
    if (!Driving(sample.flags) || sample.speed < 3) return {};
    float lock = 0, spin = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const float ratio = std::abs(sample.omega[i]) * sample.radius[i] / sample.speed;
        lock = std::max(lock, 1 - ratio);
        spin = std::max(spin, ratio - 1);
    }
    // This is wheel-lock estimation, not an ABS-active flag. The dead zones avoid
    // reacting to normal rolling-radius differences and small cornering differences.
    const float braking = std::clamp((lock - 0.08f) / 0.20f, 0.0f, 1.0f);
    const float traction = std::max(std::clamp((spin - 0.10f) / 0.30f, 0.0f, 1.0f),
                                    (sample.flags & 0x800) ? 0.65f : 0.0f);
    return {static_cast<std::uint8_t>(braking * sample.brake),
            static_cast<std::uint8_t>(traction * sample.throttle)};
}
inline std::mutex sample_mutex;
inline Sample latest_sample;
inline Clock::time_point updated{};
inline bool have_sample{};
inline std::atomic<bool> stopping{};
// FEAT-GT7-179: the car's deceleration (m/s^2, smoothed) and the end of the last gear change's
// let-off, for pedal resistance that follows the car.
inline float deceleration{};
inline Clock::time_point shift_until{};
inline void Publish(const std::uint8_t* packet) {
    Sample sample;
    if (!Decode(packet, sample)) return;
    std::lock_guard lock(sample_mutex);
    if (!have_sample || sample.packet != latest_sample.packet) {
        const auto now = Clock::now();
        const float dt = have_sample ? std::chrono::duration<float>(now - updated).count() : 0.0f;
        if (dt > 0 && dt < 0.25f) {
            const float measured = (latest_sample.speed - sample.speed) / dt;
            if (std::abs(measured) < 100) deceleration += 0.25f * (measured - deceleration);
            if (sample.gear != latest_sample.gear && sample.gear && latest_sample.gear && sample.speed > 1)
                shift_until = now + std::chrono::milliseconds(90);
        } else {
            deceleration = 0;
        }
        latest_sample = sample;
        updated = now;
        have_sample = true;
    }
}
// FEAT-GT7-179: how firm each pedal is, 0 (soft) to 1 (firmest). The brake is soft at a standstill,
// firmer when moving and firmest under hard braking (about 1 g); the accelerator stiffens with the
// revs, lets go during a gear change and pulses on the rev limiter.
inline float BrakeFirmness(float speed, float decel) {
    const float moving = std::clamp(speed / 30.0f, 0.0f, 1.0f);
    return std::clamp(0.15f + 0.55f * moving + 0.5f * std::clamp(decel / 10.0f, 0.0f, 1.0f), 0.0f, 1.0f);
}
inline float ThrottleFirmness(const Sample& sample, bool shifting, Clock::time_point now) {
    if (shifting) return 0;
    const float revs = sample.rpm_limit > 1000 ? std::clamp(sample.rpm / sample.rpm_limit, 0.0f, 1.0f) : 0.0f;
    const float firmness = 0.15f + 0.85f * revs;
    if (sample.rpm_limit > 1000 && sample.rpm >= sample.rpm_limit && sample.throttle > 128) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return (ms / 50) % 2 ? 0.05f : firmness; // about 10 pulses a second
    }
    return firmness;
}
inline Feedback Current() {
    std::lock_guard lock(sample_mutex);
    if (stopping || !have_sample || Clock::now() - updated > std::chrono::milliseconds(250)) return {};
    return Calculate(latest_sample);
}
using Effect = std::array<std::uint8_t, 11>;
inline std::uint8_t Scale(std::uint8_t strength, unsigned percent) {
    return static_cast<std::uint8_t>(std::min(255u, (strength * std::min(percent, 200u) + 50u) / 100u));
}
inline Effect ScaleEffect(Effect effect, unsigned percent) {
    percent = std::min(percent, 200u);
    if (percent == 100 || effect[0] == 5) return effect;
    if (!percent) { effect = {}; effect[0] = 5; return effect; }
    auto amplitude = [percent](unsigned strength) {
        return std::min(8u, (strength * percent + 50u) / 100u);
    };
    if (effect[0] == 0x21 || effect[0] == 0x26) {
        unsigned zones = effect[1] | (unsigned{effect[2]} << 8);
        std::uint32_t packed = 0;
        for (unsigned i = 0; i < 4; ++i) packed |= std::uint32_t{effect[3+i]} << (8*i);
        std::uint32_t scaled = 0;
        for (unsigned i = 0; i < 10; ++i) if (zones & (1u << i)) {
            const unsigned strength = amplitude(((packed >> (3*i)) & 7u) + 1);
            if (!strength) zones &= ~(1u << i);
            else scaled |= (strength - 1) << (3*i);
        }
        if (!zones) { effect = {}; effect[0] = 5; return effect; }
        effect[1] = zones & 255;
        effect[2] = (zones >> 8) & 255;
        for (unsigned i = 0; i < 4; ++i) effect[3+i] = (scaled >> (8*i)) & 255;
    } else if (effect[0] == 0x25) {
        const unsigned strength = amplitude((effect[3] & 7) + 1);
        if (!strength) { effect = {}; effect[0] = 5; return effect; }
        effect[3] = strength - 1;
    }
    return effect;
}
inline Effect Vibration(std::uint8_t strength) {
    Effect effect{};
    effect[0] = 0x05; // Release resistance/vibration, including on pause and stale data.
    if (!strength) return effect;
    effect[0] = 0x26;
    effect[1] = 0xfe; // Zones 1-9; leave the start of the travel free.
    effect[2] = 0x03;
    // FEAT-GT7-179: any lock or slip is at least amplitude 3 of 7, so it is felt under the body
    // haptics instead of being a faint tremor at light pressure.
    const unsigned amplitude = 3 + (strength - 1) * 4 / 254;
    std::uint32_t packed = 0;
    for (unsigned zone = 1; zone < 10; ++zone) packed |= amplitude << (3*zone);
    for (unsigned i = 0; i < 4; ++i) effect[3+i] = (packed >> (8*i)) & 255;
    effect[9] = 25; // Native trigger vibration frequency, not a software rumble loop.
    return effect;
}
inline std::array<std::uint8_t, 47> DualSensePacket(const Effect& left, const Effect& right) {
    std::array<std::uint8_t, 47> packet{};
    packet[0] = 0x0c; // Enable ONLY right/left trigger effects; preserve LEDs/body rumble.
    std::copy(right.begin(), right.end(), packet.begin() + 10);
    std::copy(left.begin(), left.end(), packet.begin() + 21);
    return packet;
}
// A pedal whose firmness moves both where the resistance starts and how strong it gets: soft, it
// starts deep in the pull and stays light; firm, it starts near the top and builds to full force.
// The trigger has 10 zones and 8 force steps, so smaller changes would not be felt.
inline Effect DynamicResistance(bool brake, unsigned percent, float firmness) {
    firmness = std::clamp(firmness, 0.0f, 1.0f);
    percent = std::min(percent, 200u);
    if (!percent) {
        Effect release{};
        release[0] = 5;
        return release;
    }
    const unsigned start = static_cast<unsigned>(std::lround(6 - 5 * firmness)); // zone 6 .. 1
    const float peak = (brake ? 7.0f : 4.5f) * (0.45f + 0.85f * firmness) * percent / 100.0f;
    Effect effect{};
    effect[0] = 0x21;
    unsigned zones = 0;
    std::uint32_t packed = 0;
    for (unsigned i = start; i < 10; ++i) {
        const float linear = static_cast<float>(i - start + 1) / static_cast<float>(10 - start);
        // The brake's force builds quickly once it engages, as a real pedal's does.
        const float ramp = brake ? std::sqrt(linear) : linear;
        const unsigned force = std::min(8u, static_cast<unsigned>(std::lround(peak * ramp)));
        if (!force) continue;
        zones |= 1u << i;
        packed |= (force - 1) << (3 * i);
    }
    if (!zones) {
        effect = {};
        effect[0] = 5;
        return effect;
    }
    effect[1] = zones & 255;
    effect[2] = (zones >> 8) & 255;
    for (unsigned i = 0; i < 4; ++i) effect[3 + i] = (packed >> (8 * i)) & 255;
    return effect;
}
inline Effect PedalResistance(bool brake, unsigned percent) {
    // Increasing force through the travel, with the initial zone free. The brake
    // is firmer than the accelerator; percentages scale force, not zone positions.
    constexpr std::array<unsigned, 10> brake_force{0, 1, 1, 2, 2, 3, 4, 5, 6, 7};
    constexpr std::array<unsigned, 10> throttle_force{0, 1, 1, 1, 1, 2, 2, 2, 3, 3};
    Effect effect{};
    effect[0] = 0x21;
    unsigned zones = 0;
    std::uint32_t packed = 0;
    for (unsigned i = 0; i < 10; ++i) {
        const unsigned force = std::min(8u, ((brake ? brake_force[i] : throttle_force[i]) *
                                                  std::min(percent, 200u) + 50) / 100);
        if (!force) continue;
        zones |= 1u << i;
        packed |= (force - 1) << (3*i);
    }
    if (!zones) return Vibration(0);
    effect[1] = zones & 255;
    effect[2] = (zones >> 8) & 255;
    for (unsigned i = 0; i < 4; ++i) effect[3+i] = (packed >> (8*i)) & 255;
    return effect;
}
struct Output {
    Effect left = Vibration(0), right = Vibration(0);
    std::uint8_t left_rumble{}, right_rumble{};
};
// FEAT-GT7-179: how much the resistance follows the car: 0 keeps it at a fixed middle firmness,
// 100 as designed, 200 twice as far from the middle.
inline float WithDynamic(float firmness, unsigned dynamic) {
    return std::clamp(0.5f + (firmness - 0.5f) * static_cast<float>(std::min(dynamic, 200u)) / 100.0f,
                      0.0f, 1.0f);
}
// The trigger effects for one telemetry sample, with the launcher's settings: wheel lock and spin
// vibrate (each scaled by its own setting, 0 = no vibration, the resistance stays), otherwise each
// pedal resists by its own setting, firmer with speed, braking and revs as far as trigger_dynamic
// allows. Pure, so ryty-feel-test plays exactly what the game does.
inline Output Compute(const Sample& sample, float decel, bool shifting, Clock::time_point now,
                      const Feel::Settings& settings) {
    if (!Driving(sample.flags)) return {};
    const auto feedback = Calculate(sample);
    const unsigned percent = std::min(settings.trigger, 200u);
    Output output;
    output.left_rumble = Scale(feedback.left, percent * settings.lock / 100);
    output.right_rumble = Scale(feedback.right, percent * settings.spin / 100);
    output.left = output.left_rumble
                      ? Vibration(output.left_rumble)
                      : DynamicResistance(true, percent * settings.brake / 100,
                                          WithDynamic(BrakeFirmness(sample.speed, decel), settings.dynamic));
    output.right = output.right_rumble
                       ? Vibration(output.right_rumble)
                       : DynamicResistance(false, percent * settings.throttle / 100,
                                           WithDynamic(ThrottleFirmness(sample, shifting, now), settings.dynamic));
    return output;
}
inline Output CurrentOutput(const Feel::Settings& settings) {
    std::lock_guard lock(sample_mutex);
    if (stopping || !have_sample || Clock::now() - updated > std::chrono::milliseconds(250)) return {};
    const auto now = Clock::now();
    return Compute(latest_sample, deceleration, now < shift_until, now, settings);
}
inline Output CurrentOutput(unsigned percent) {
    Feel::Settings settings;
    settings.trigger = percent;
    return CurrentOutput(settings);
}
} // namespace Input::TriggerFeedback
