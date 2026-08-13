// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/AudioImportPolicy.h"
#include "engine/nodes/AudioClipNode.h"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#include <cstdio>
#include <filesystem>

namespace dave::engine {

bool AudioClipNode::loadFromFile(const std::string& path) {
    // UI thread. Decode the whole file into memory. For RB-1 this is fine;
    // streaming from disk (for long files) comes with the disk I/O pool later.
    std::error_code sizeError;
    const auto encodedBytes = std::filesystem::file_size(path, sizeError);
    if (!sizeError && !audio::canDecodeFileInMemory(encodedBytes)) {
        std::fprintf(stderr,
                     "Dave: WAV import refused: files larger than 4 GiB "
                     "require streaming playback: %s\n",
                     path.c_str());
        return false;
    }

    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) {
        std::fprintf(stderr, "Dave: failed to open WAV: %s\n", path.c_str());
        return false;
    }

    // Decode as interleaved float first, then deinterleave.
    const drwav_uint64 totalFrames = wav.totalPCMFrameCount;
    const drwav_uint32 channels = wav.channels;
    std::vector<float> interleaved(static_cast<size_t>(totalFrames) * channels);
    const drwav_uint64 decoded = drwav_read_pcm_frames_f32(
        &wav, totalFrames, interleaved.data());
    drwav_uninit(&wav);

    if (decoded == 0 || channels == 0) {
        std::fprintf(stderr, "Dave: WAV decoded zero frames: %s\n", path.c_str());
        return false;
    }

    // Deinterleave into per-channel vectors.
    std::vector<std::vector<float>> deinterleaved(channels);
    for (auto& ch : deinterleaved) {
        ch.resize(static_cast<size_t>(decoded));
    }
    for (drwav_uint64 i = 0; i < decoded; ++i) {
        for (drwav_uint32 c = 0; c < channels; ++c) {
            deinterleaved[c][i] = interleaved[static_cast<size_t>(i) * channels + c];
        }
    }

    ownedBuffer_ = std::move(deinterleaved);
    buffer_ = &ownedBuffer_;
    sourceSampleRate_ = static_cast<double>(wav.sampleRate);
    return true;
}

} // namespace dave::engine
