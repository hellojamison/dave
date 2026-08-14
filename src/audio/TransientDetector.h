// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/DecodedAudioAsset.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace dave::audio {

struct TransientCandidate {
    int64_t sourceSample = 0;
    float strength = 0.0f;

    bool operator==(const TransientCandidate&) const = default;
};

class TransientDetector {
public:
    // Produces a sensitivity-independent candidate set. `cancelGeneration`
    // lets the background cache stop a long scan without coupling this pure
    // DSP code to the application or UI.
    static std::vector<TransientCandidate> analyze(
        const DecodedAudioAsset& asset,
        const std::atomic<uint64_t>* cancelGeneration = nullptr,
        uint64_t expectedGeneration = 0);

    // Higher sensitivity includes every candidate accepted by lower
    // sensitivity plus weaker candidates. The input remains untouched.
    static std::vector<TransientCandidate> filterForSensitivity(
        const std::vector<TransientCandidate>& candidates,
        int sensitivity);

    static float thresholdForSensitivity(int sensitivity) noexcept;
};

} // namespace dave::audio
