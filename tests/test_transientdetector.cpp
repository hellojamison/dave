// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/TransientDetector.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace dave;

namespace {

audio::DecodedAudioAsset impulseAsset(double sampleRate,
                                      std::initializer_list<double> seconds,
                                      float amplitude = 1.0f,
                                      int channels = 1) {
    audio::DecodedAudioAsset asset;
    asset.sampleRate = sampleRate;
    const size_t frames = static_cast<size_t>(sampleRate * 0.5);
    asset.channels.assign(static_cast<size_t>(channels),
                          std::vector<float>(frames, 0.0f));
    for (double time : seconds) {
        const size_t sample = static_cast<size_t>(std::llround(time * sampleRate));
        if (sample < frames) asset.channels.back()[sample] = amplitude;
    }
    return asset;
}

bool nearSample(const std::vector<audio::TransientCandidate>& candidates,
                int64_t expected, int64_t tolerance) {
    return std::any_of(candidates.begin(), candidates.end(), [&](const auto& item) {
        return std::abs(item.sourceSample - expected) <= tolerance;
    });
}

} // namespace

TEST_CASE("transient detector finds separated impulses deterministically",
          "[transient][detector]") {
    const auto asset = impulseAsset(48000.0, {0.05, 0.20, 0.35});
    const auto first = audio::TransientDetector::analyze(asset);
    const auto second = audio::TransientDetector::analyze(asset);
    REQUIRE(first == second);
    REQUIRE(nearSample(first, 2400, 96));
    REQUIRE(nearSample(first, 9600, 96));
    REQUIRE(nearSample(first, 16800, 96));
}

TEST_CASE("transient detector preserves attacks isolated to any channel",
          "[transient][detector]") {
    auto asset = impulseAsset(48000.0, {0.125}, 1.0f, 2);
    REQUIRE(std::all_of(asset.channels.front().begin(),
                        asset.channels.front().end(),
                        [](float value) { return value == 0.0f; }));
    const auto candidates = audio::TransientDetector::analyze(asset);
    REQUIRE(nearSample(candidates, 6000, 96));
}

TEST_CASE("transient detector is sample-rate-scaled and bounded",
          "[transient][detector]") {
    for (double sampleRate : {44100.0, 48000.0, 96000.0}) {
        const auto asset = impulseAsset(sampleRate, {0.1});
        const auto candidates = audio::TransientDetector::analyze(asset);
        REQUIRE(nearSample(candidates,
                           static_cast<int64_t>(std::llround(sampleRate * 0.1)),
                           static_cast<int64_t>(sampleRate * 0.003)));
        REQUIRE(std::all_of(candidates.begin(), candidates.end(),
                            [](const auto& candidate) {
                                return candidate.strength >= 0.0f &&
                                       candidate.strength <= 1.0f;
                            }));
    }
}

TEST_CASE("silence and malformed channel bounds are safe",
          "[transient][detector]") {
    audio::DecodedAudioAsset silence;
    silence.sampleRate = 48000.0;
    silence.channels = {std::vector<float>(4800, 0.0f)};
    REQUIRE(audio::TransientDetector::analyze(silence).empty());

    audio::DecodedAudioAsset uneven;
    uneven.sampleRate = 48000.0;
    uneven.channels = {std::vector<float>(200, 0.0f),
                       std::vector<float>(50, 0.0f)};
    uneven.channels[0][100] = 1.0f; // outside the common safe frame count
    REQUIRE(audio::TransientDetector::analyze(uneven).empty());
}

TEST_CASE("higher sensitivity is a monotonic superset",
          "[transient][detector]") {
    auto strong = audio::TransientDetector::analyze(
        impulseAsset(48000.0, {0.05, 0.20}, 1.0f));
    strong.push_back({20000, 0.08f});
    const auto low = audio::TransientDetector::filterForSensitivity(strong, 10);
    const auto medium = audio::TransientDetector::filterForSensitivity(strong, 50);
    const auto high = audio::TransientDetector::filterForSensitivity(strong, 100);
    REQUIRE(low.size() <= medium.size());
    REQUIRE(medium.size() <= high.size());
    REQUIRE(std::find(high.begin(), high.end(),
                      audio::TransientCandidate{20000, 0.08f}) != high.end());
}

TEST_CASE("attack strength tracks soft versus loud impulses",
          "[transient][detector]") {
    const auto soft = audio::TransientDetector::analyze(
        impulseAsset(48000.0, {0.1}, 0.08f));
    const auto loud = audio::TransientDetector::analyze(
        impulseAsset(48000.0, {0.1}, 1.0f));
    REQUIRE_FALSE(soft.empty());
    REQUIRE_FALSE(loud.empty());
    REQUIRE(loud.front().strength > soft.front().strength);
}

TEST_CASE("steady content does not create a stream of false onsets",
          "[transient][detector]") {
    audio::DecodedAudioAsset tone;
    tone.sampleRate = 48000.0;
    tone.channels = {std::vector<float>(24000, 0.0f)};
    for (size_t sample = 0; sample < tone.channels[0].size(); ++sample) {
        tone.channels[0][sample] = 0.3f * std::sin(
            2.0 * 3.141592653589793 * 440.0 *
            static_cast<double>(sample) / tone.sampleRate);
    }
    const auto candidates = audio::TransientDetector::analyze(tone);
    REQUIRE(std::count_if(candidates.begin(), candidates.end(),
                          [](const auto& candidate) {
                              return candidate.sourceSample > 2400;
                          }) <= 1);
}

TEST_CASE("deterministic noise candidates respect the refractory interval",
          "[transient][detector]") {
    audio::DecodedAudioAsset noise;
    noise.sampleRate = 48000.0;
    noise.channels = {std::vector<float>(24000, 0.0f)};
    uint32_t state = 0x12345678u;
    for (float& sample : noise.channels[0]) {
        state = state * 1664525u + 1013904223u;
        sample = (static_cast<float>((state >> 8) & 0xffffu) / 32767.5f - 1.0f) *
            0.01f;
    }
    const auto candidates = audio::TransientDetector::analyze(noise);
    for (size_t index = 1; index < candidates.size(); ++index) {
        REQUIRE(candidates[index].sourceSample -
                    candidates[index - 1].sourceSample >= 480);
    }
}
