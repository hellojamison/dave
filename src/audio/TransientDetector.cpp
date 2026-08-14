// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/TransientDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dave::audio {

namespace {

bool cancelled(const std::atomic<uint64_t>* generation,
               uint64_t expected) noexcept {
    return generation != nullptr &&
        generation->load(std::memory_order_relaxed) != expected;
}

size_t commonFrameCount(const DecodedAudioAsset& asset) {
    if (asset.channels.empty()) return 0;
    size_t frames = std::numeric_limits<size_t>::max();
    for (const auto& channel : asset.channels) {
        frames = std::min(frames, channel.size());
    }
    return frames == std::numeric_limits<size_t>::max() ? 0 : frames;
}

} // namespace

std::vector<TransientCandidate> TransientDetector::analyze(
    const DecodedAudioAsset& asset,
    const std::atomic<uint64_t>* cancelGeneration,
    uint64_t expectedGeneration) {
    std::vector<TransientCandidate> result;
    const size_t frames = commonFrameCount(asset);
    if (frames < 3 || asset.sampleRate <= 0.0 || asset.channels.empty()) {
        return result;
    }

    // About one novelty value per millisecond. The bounds keep very unusual
    // file sample rates from producing pathological work or coarse timing.
    const size_t hop = static_cast<size_t>(std::clamp(
        std::llround(asset.sampleRate * 0.001), 16LL, 512LL));
    const size_t hopCount = (frames + hop - 1) / hop;
    std::vector<float> novelty(hopCount, 0.0f);

    const float fastDecay = static_cast<float>(
        std::exp(-static_cast<double>(hop) / (asset.sampleRate * 0.008)));
    const float slowBlend = static_cast<float>(
        1.0 - std::exp(-static_cast<double>(hop) / (asset.sampleRate * 0.050)));
    float fast = 0.0f;
    float slow = 0.0f;
    float previousDrive = 0.0f;

    for (size_t block = 0; block < hopCount; ++block) {
        if ((block & 0x3ffu) == 0 &&
            cancelled(cancelGeneration, expectedGeneration)) {
            return {};
        }
        const size_t begin = block * hop;
        const size_t end = std::min(frames, begin + hop);
        float strongestEnergy = 0.0f;
        float strongestDifference = 0.0f;
        for (const auto& channel : asset.channels) {
            double energy = 0.0;
            double difference = 0.0;
            float previous = begin > 0 ? channel[begin - 1] : channel[begin];
            for (size_t sample = begin; sample < end; ++sample) {
                const float value = std::isfinite(channel[sample])
                    ? channel[sample] : 0.0f;
                energy += static_cast<double>(value) * value;
                difference += std::abs(value - previous);
                previous = value;
            }
            const double count = static_cast<double>(std::max<size_t>(1, end - begin));
            strongestEnergy = std::max(
                strongestEnergy, static_cast<float>(std::sqrt(energy / count)));
            strongestDifference = std::max(
                strongestDifference, static_cast<float>(difference / count));
        }

        // The derivative term catches short attacks that RMS alone smears;
        // the slow envelope supplies an adaptive local floor. Neither channel
        // downmixes, so opposite-polarity stereo content cannot cancel out.
        const float drive = strongestEnergy + strongestDifference * 1.5f;
        fast = std::max(drive, fast * fastDecay);
        slow += (drive - slow) * slowBlend;
        const float contrast = std::max(0.0f, fast - slow);
        const float normalizedContrast =
            contrast / (fast + slow + 0.01f);
        const float positiveFlux = std::max(0.0f, drive - previousDrive);
        const float normalizedFlux =
            positiveFlux / (drive + previousDrive + 0.01f);
        novelty[block] = std::clamp(
            std::sqrt(normalizedContrast * normalizedFlux), 0.0f, 1.0f);
        previousDrive = drive;
    }

    const int64_t minimumSpacing = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(asset.sampleRate * 0.010)));
    // Below this contrast, hop-to-hop phase differences in a steady pitched
    // tone look like tiny local maxima even though no new event occurred.
    // Real attacks (including quiet ones) rise far above this adaptive ratio.
    constexpr float candidateFloor = 0.08f;
    for (size_t block = 0; block < hopCount; ++block) {
        const float value = novelty[block];
        const float before = block == 0 ? 0.0f : novelty[block - 1];
        const float after = block + 1 == hopCount ? 0.0f : novelty[block + 1];
        if (value < candidateFloor || value < before || value < after) continue;

        const size_t coarse = block * hop;
        const size_t searchBegin = coarse > hop ? coarse - hop : 1;
        const size_t searchEnd = std::min(frames, coarse + hop + 1);
        size_t bestSample = searchBegin;
        float bestDifference = -1.0f;
        for (size_t sample = searchBegin; sample < searchEnd; ++sample) {
            float difference = 0.0f;
            for (const auto& channel : asset.channels) {
                const float current = std::isfinite(channel[sample])
                    ? channel[sample] : 0.0f;
                const float previous = std::isfinite(channel[sample - 1])
                    ? channel[sample - 1] : 0.0f;
                difference = std::max(difference, std::abs(current - previous));
            }
            if (difference > bestDifference) {
                bestDifference = difference;
                bestSample = sample;
            }
        }

        TransientCandidate candidate{
            static_cast<int64_t>(bestSample), value};
        if (!result.empty() &&
            candidate.sourceSample - result.back().sourceSample < minimumSpacing) {
            if (candidate.strength > result.back().strength) {
                result.back() = candidate;
            }
        } else {
            result.push_back(candidate);
        }
    }
    return result;
}

float TransientDetector::thresholdForSensitivity(int sensitivity) noexcept {
    const float normalized = static_cast<float>(std::clamp(sensitivity, 0, 100)) /
        100.0f;
    return 0.92f - normalized * 0.885f;
}

std::vector<TransientCandidate> TransientDetector::filterForSensitivity(
    const std::vector<TransientCandidate>& candidates,
    int sensitivity) {
    const float threshold = thresholdForSensitivity(sensitivity);
    std::vector<TransientCandidate> filtered;
    filtered.reserve(candidates.size());
    std::copy_if(candidates.begin(), candidates.end(),
                 std::back_inserter(filtered),
                 [threshold](const TransientCandidate& candidate) {
                     return candidate.strength >= threshold;
                 });
    return filtered;
}

} // namespace dave::audio
