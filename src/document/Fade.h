// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

namespace dave::document {

// The shape of a clip fade. The gain functions below turn a shape and a
// normalized position into a multiplier; the same code paints the curve in the
// timeline and applies it on the audio thread, so what you see is what you
// hear.
//
// Kept header-only and branch-light on purpose: AudioClipNode::process calls
// fadeInGain once per sample inside its inner loop.
enum class FadeShape : std::uint8_t {
    Linear,      // straight line
    EqualPower,  // sin/cos law — constant power, the safe default for crossfades
    Slow,        // convex: starts slow, ends fast (a gentle "fade up")
    Fast,        // concave: starts fast, ends slow
    SCurve,      // eased at both ends (smoothstep)
};

// Gain for a fade-IN at normalized position `t` in [0, 1]: silent (0) at the
// clip edge, unity (1) once the fade completes. `t` is clamped, so callers can
// pass a raw ratio without guarding the ends.
inline float fadeInGain(FadeShape shape, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (shape) {
        case FadeShape::Linear:
            return t;
        case FadeShape::EqualPower:
            return std::sin(t * 1.57079632679489661923f);  // sin(t·π/2)
        case FadeShape::Slow:
            return t * t;                                   // convex
        case FadeShape::Fast:
            return t * (2.0f - t);                          // 1 - (1-t)^2
        case FadeShape::SCurve:
            return t * t * (3.0f - 2.0f * t);               // smoothstep
    }
    return t;
}

// Gain for a fade-OUT at normalized position `t` in [0, 1]: unity (1) where the
// fade begins, silent (0) at the clip's end. Defined as the fade-in curve read
// backwards, so a shape's in and out are mirror images — an equal-power out is
// the constant-power partner of an equal-power in.
inline float fadeOutGain(FadeShape shape, float t) {
    return fadeInGain(shape, 1.0f - t);
}

} // namespace dave::document
