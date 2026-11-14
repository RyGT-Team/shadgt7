// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-GT7-179: plays telemetry_haptics.h's effects on a USB DualSense's actuators.
#pragma once
#include <cstdint>

namespace Input::TelemetryHaptics {
// Called with each Simulator Interface packet (GT Sport's and GT7's telemetry hook). The first
// call starts looking for a DualSense sound card. Strength comes from the launcher's HD rumble
// settings (Input::Feel); 0 silences an effect.
void Publish(const std::uint8_t* packet);
} // namespace Input::TelemetryHaptics
