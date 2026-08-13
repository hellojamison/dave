// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dave::platform {

// Input choice is deliberately separate from the capture-device catalog.
// Off and Default remain meaningful even when no named devices enumerate.
struct InputDeviceSelection {
    enum class Kind {
        Off,
        Default,
        Device,
    };

    Kind kind = Kind::Off;
    int deviceIndex = -1;

    static constexpr InputDeviceSelection off() { return {}; }
    static constexpr InputDeviceSelection defaultDevice() {
        return {Kind::Default, -1};
    }
    static constexpr InputDeviceSelection device(int index) {
        return {Kind::Device, index};
    }
};

// A pure description of the device-open sequence. Keeping this independent
// of miniaudio gives headless tests a hardware-free way to prove that capture
// requests use native channel counts and always retain a playback-only
// fallback attempt.
struct AudioDeviceOpenAttempt {
    bool duplex = false;
    std::uint32_t sampleRate = 0;
    std::uint32_t playbackChannels = 0;
    // Zero is miniaudio's "use the device's native channel count" request.
    std::uint32_t captureChannels = 0;
};

struct AudioDeviceOpenPlan {
    std::array<AudioDeviceOpenAttempt, 2> attempts{};
    std::size_t count = 0;
};

constexpr AudioDeviceOpenPlan makeAudioDeviceOpenPlan(
    InputDeviceSelection input, std::uint32_t sampleRate,
    std::uint32_t playbackChannels) {
    (void)playbackChannels;
    constexpr std::uint32_t nativePlaybackChannels = 0;
    AudioDeviceOpenPlan plan;
    if (input.kind == InputDeviceSelection::Kind::Off) {
        plan.attempts[0] = {false, sampleRate, nativePlaybackChannels, 0};
        plan.count = 1;
        return plan;
    }

    plan.attempts[0] = {true, sampleRate, nativePlaybackChannels, 0};
    plan.attempts[1] = {false, sampleRate, nativePlaybackChannels, 0};
    plan.count = 2;
    return plan;
}

} // namespace dave::platform
