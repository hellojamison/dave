// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fade-curve math. The same functions paint the timeline and gain the
// audio thread, so their shape has to be exactly right at the edges (no click)
// and monotonic in between (no dip). These are pure functions; the interesting
// properties are the endpoints, monotonicity, and — for equal power — that a
// crossfade holds constant power.
#include "document/Fade.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using dave::document::FadeShape;
using dave::document::fadeInGain;
using dave::document::fadeOutGain;

namespace {
constexpr FadeShape kAll[] = {FadeShape::Linear, FadeShape::EqualPower,
                              FadeShape::Slow,   FadeShape::Fast,
                              FadeShape::SCurve};
}

TEST_CASE("every fade shape is silent at one edge and unity at the other",
          "[fade]") {
    for (FadeShape s : kAll) {
        // A fade-in that isn't exactly 0 at the head clicks; exactly 1 at the
        // tail or the clip steps in level where the fade ends.
        CHECK(fadeInGain(s, 0.0f) == 0.0f);
        CHECK(fadeInGain(s, 1.0f) == 1.0f);
        CHECK(fadeOutGain(s, 0.0f) == 1.0f);
        CHECK(fadeOutGain(s, 1.0f) == 0.0f);
        // Out of range is clamped, not extrapolated.
        CHECK(fadeInGain(s, -0.5f) == 0.0f);
        CHECK(fadeInGain(s, 1.5f) == 1.0f);
    }
}

TEST_CASE("every fade shape rises monotonically", "[fade]") {
    for (FadeShape s : kAll) {
        float prev = -1.0f;
        for (int i = 0; i <= 100; ++i) {
            const float g = fadeInGain(s, i / 100.0f);
            CHECK(g >= prev);  // never dips
            prev = g;
        }
    }
}

TEST_CASE("fade out is the fade in read backwards", "[fade]") {
    for (FadeShape s : kAll) {
        for (int i = 0; i <= 10; ++i) {
            const float t = i / 10.0f;
            CHECK(std::abs(fadeOutGain(s, t) - fadeInGain(s, 1.0f - t)) < 1e-6f);
        }
    }
}

TEST_CASE("the shapes bend the way their names say", "[fade]") {
    // Linear is the diagonal.
    CHECK(std::abs(fadeInGain(FadeShape::Linear, 0.5f) - 0.5f) < 1e-6f);
    // Slow starts below the diagonal (convex), Fast above it (concave).
    CHECK(fadeInGain(FadeShape::Slow, 0.5f) < 0.5f);
    CHECK(fadeInGain(FadeShape::Fast, 0.5f) > 0.5f);
    // The S-curve crosses the diagonal at the midpoint.
    CHECK(std::abs(fadeInGain(FadeShape::SCurve, 0.5f) - 0.5f) < 1e-6f);
    // Equal power sits at 1/√2 halfway.
    CHECK(std::abs(fadeInGain(FadeShape::EqualPower, 0.5f) - 0.70710678f) < 1e-5f);
}

TEST_CASE("an equal-power crossfade holds constant power", "[fade]") {
    // The reason equal power exists: summed power across the crossfade stays
    // flat, so a crossfade neither dips nor bumps in the middle. Linear fails
    // this (it dips ~3 dB), which is the whole point of offering the choice.
    for (int i = 0; i <= 10; ++i) {
        const float t = i / 10.0f;
        const float in = fadeInGain(FadeShape::EqualPower, t);
        const float out = fadeOutGain(FadeShape::EqualPower, t);
        CHECK(std::abs(in * in + out * out - 1.0f) < 1e-5f);
    }
}
