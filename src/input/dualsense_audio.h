// SPDX-License-Identifier: GPL-2.0-or-later
// FEAT-GT7-179: a USB DualSense's sound card, whose channels 3 and 4 drive the left and right
// voice-coil actuators. Shared by the renderer's telemetry haptics and ryty-feel-test.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <SDL3/SDL.h>

namespace Input::DualSenseAudio {

constexpr int SampleRate = 48000;

struct Device {
    SDL_AudioStream* stream{};
    SDL_AudioDeviceID id{};
    int channels{};
    int left{2}, right{3};
    std::string name;
};

inline bool IsDualSense(const char* name) {
    if (!name) return false;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("dualsense") != std::string::npos;
}

// Opens the first DualSense sound card as a 48 kHz float stream fed by callback. The stream is
// left paused, so the caller can finish setting up before calling SDL_ResumeAudioStreamDevice.
// On failure, error says why (empty when there is simply no DualSense on USB).
inline bool Open(Device& device, SDL_AudioStreamCallback callback, void* user, std::string& error) {
    error.clear();
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (!devices) {
        error = SDL_GetError();
        return false;
    }
    bool opened = false;
    for (int i = 0; i < count && !opened; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (!IsDualSense(name)) continue;
        SDL_AudioSpec native{};
        int frames = 0;
        if (!SDL_GetAudioDeviceFormat(devices[i], &native, &frames)) continue;
        // Four channels; PulseAudio shows the card as 5.1 with the actuators on the rear pair.
        int left = 2, right = 3;
        if (native.channels == 6) {
            left = 4;
            right = 5;
        } else if (native.channels != 4) {
            continue;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, native.channels, SampleRate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(devices[i], &spec, callback, user);
        if (!stream) {
            error = std::string("could not open '") + name + "': " + SDL_GetError();
            continue;
        }
        device.stream = stream;
        device.id = devices[i];
        device.channels = native.channels;
        device.left = left;
        device.right = right;
        device.name = name;
        opened = true;
    }
    SDL_free(devices);
    return opened;
}

inline bool Present(SDL_AudioDeviceID id) {
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (!devices) return false;
    const bool present = std::find(devices, devices + count, id) != devices + count;
    SDL_free(devices);
    return present;
}

inline void Close(Device& device) {
    if (device.stream) SDL_DestroyAudioStream(device.stream);
    device = {};
}

} // namespace Input::DualSenseAudio
