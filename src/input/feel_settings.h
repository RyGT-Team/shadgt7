// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-GT7-179: the launcher's "HD rumble & adaptive triggers" settings. Each is a percentage from
// 0 (that effect off) to 200 (boosted), 100 standard, stored in the game config's Input object and
// read again every second while playing. ryty-feel-test takes the same names as name=value
// arguments, so the launcher's Test buttons play with the settings on screen.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

namespace Input::Feel {

struct Settings {
    // Adaptive triggers.
    unsigned trigger = 100;  // trigger_strength: every trigger effect
    unsigned brake = 100;    // trigger_brake: brake pedal resistance
    unsigned throttle = 100; // trigger_throttle: accelerator resistance
    unsigned dynamic = 100;  // trigger_dynamic: how much the resistance follows speed, braking, revs
    unsigned lock = 100;     // trigger_lock: wheel-lock (ABS) vibration on the brake trigger
    unsigned spin = 100;     // trigger_spin: wheelspin and traction-control vibration on the accelerator
    // HD rumble (the controller body).
    unsigned haptics = 100;  // haptics_strength: every HD rumble effect
    unsigned engine = 100;   // haptics_engine: the hum that follows the revs
    unsigned road = 100;     // haptics_road: kerbs and bumps
    unsigned slip = 100;     // haptics_slip: wheel lock and wheelspin
    unsigned shift = 100;    // haptics_shift: gear changes
    unsigned limiter = 100;  // haptics_limiter: the rev limiter
    unsigned impact = 100;   // haptics_impact: crashes and hard hits
};

struct Key {
    const char* name;
    unsigned Settings::*field;
};
inline constexpr Key Keys[] = {
    {"trigger_strength", &Settings::trigger}, {"trigger_brake", &Settings::brake},
    {"trigger_throttle", &Settings::throttle}, {"trigger_dynamic", &Settings::dynamic},
    {"trigger_lock", &Settings::lock},         {"trigger_spin", &Settings::spin},
    {"haptics_strength", &Settings::haptics},  {"haptics_engine", &Settings::engine},
    {"haptics_road", &Settings::road},         {"haptics_slip", &Settings::slip},
    {"haptics_shift", &Settings::shift},       {"haptics_limiter", &Settings::limiter},
    {"haptics_impact", &Settings::impact},
};

inline unsigned Clamp(long long value) {
    return static_cast<unsigned>(std::clamp<long long>(value, 0, 200));
}

// Sets one setting from "name=value"; false if the name is unknown or the value not a number.
inline bool ParseArgument(std::string_view argument, Settings& settings) {
    const auto equals = argument.find('=');
    if (equals == std::string_view::npos) return false;
    const auto name = argument.substr(0, equals);
    const std::string value(argument.substr(equals + 1));
    if (value.empty()) return false;
    char* end = nullptr;
    const long long number = std::strtoll(value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    for (const auto& key : Keys) {
        if (name == key.name) {
            settings.*key.field = Clamp(number);
            return true;
        }
    }
    return false;
}

// The settings in effect, set by the input bridge from the game config.
inline std::mutex mutex;
inline Settings current;
inline Settings Get() {
    std::lock_guard lock(mutex);
    return current;
}
inline void Set(const Settings& settings) {
    std::lock_guard lock(mutex);
    current = settings;
}

} // namespace Input::Feel
