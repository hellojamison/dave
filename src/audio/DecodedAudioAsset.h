// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <vector>

namespace dave::audio {

// One immutable in-memory decode shared by playback and derived UI analysis.
// Owners retain the shared pointer off the real-time thread; audio callbacks
// only dereference the already-retained object.
struct DecodedAudioAsset {
    std::vector<std::vector<float>> channels;
    double sampleRate = 48000.0;
};

using DecodedAudioAssetPtr = std::shared_ptr<const DecodedAudioAsset>;

} // namespace dave::audio
