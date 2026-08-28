// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/nodes/GainNode.h"

#include <imgui.h>

namespace dave::gui {

// The post-fader level meter, shared by the mixer strip and the timeline's
// track headers.
//
// It lives here rather than in either caller because the two show the same
// signal at the same point in the chain: a second implementation would drift
// in its dB scale or its clip threshold, and a meter that disagrees with the
// meter next to it is worse than one meter.
//
// The scale is -60 to +6 dBFS, matching the fader's own range.
// What the meter is showing. Global rather than per-track: a bank of meters
// where some read pre-fader and some post cannot be compared at a glance,
// which is the only reason to have a bank.
struct LevelMeterOptions {
    // Pre-fader reads the source before gain, pan and automation — useful for
    // setting a level; post-fader reads what is actually leaving the track.
    bool preFader = false;
    // The bar body. RMS tracks loudness, linear peak tracks the sample values
    // that will clip. The peak line is drawn either way.
    bool rmsBody = true;
    // How long the peak marker sits at its maximum, in seconds. Negative holds
    // it until the clip latch is cleared; zero lets it follow the bar. The
    // choices the menu offers are kMeterPeakHoldChoices below.
    float peakHoldSeconds = -1.0f;
    // Where the channel strip puts its meter: in the signal chain at the fader
    // position (false, the default), or as a dedicated bar directly below the
    // fader control (true). A layout choice, not a metering one, but it rides
    // here so the meter's own menu can offer it and it persists with the rest.
    bool belowFader = false;
};

// The hold times the meter menu and Preferences offer. Infinite is last
// because it is the default and reads as "off" for the falling behaviour.
struct MeterPeakHoldChoice {
    const char* label;
    float seconds;
};
inline constexpr MeterPeakHoldChoice kMeterPeakHoldChoices[] = {
    {"Follow bar", 0.0f},
    {"1 second", 1.0f},
    {"2 seconds", 2.0f},
    {"5 seconds", 5.0f},
    {"Until cleared", -1.0f},
};

struct LevelMeterStyle {
    float channelWidth = 7.0f;
    float channelGap = 3.0f;
    // A clip indicator needs the room; below this it is dropped rather than
    // drawn as an unreadable smear.
    float minHeightForClipDot = 24.0f;
    bool showTooltip = true;
};

// Total width for `channels` bars in this style.
float levelMeterWidth(const LevelMeterStyle& style, int channels = 2);

// Map an amplitude to a Y inside [top, bottom]. Exposed so callers can align
// scale marks with the bars.
float amplitudeToMeterY(float amplitude, float top, float bottom);

// 32-bit is float in this document; 16 and 24 are fixed point. Written once
// because three separate meters ask the question and a fourth will.
inline bool sessionHasFloatHeadroom(int bitDepth) { return bitDepth >= 32; }

// The meter's dB scale. Named because the headroom band's arithmetic depends
// on both ends of it and a mismatch would silently mis-size the warning.
inline constexpr float kMeterFloorDb = -60.0f;
inline constexpr float kMeterCeilingDb = 6.0f;

// How far a held peak went above 0 dBFS, or 0 when it never did.
//
// In a 32-bit float session this is not damage — nothing clipped, and pulling
// the fader down recovers it exactly. What it costs is at the OTHER end: the
// same amount of quiet detail is pushed below what the session's fixed-point
// render can represent, and that part is gone for good. So the over reads as
// information and the loss it implies reads as damage.
float meterOverDb(float heldPeak);

// The height of the doomed noise floor as a fraction of the meter, given that
// much over. Clamped to the meter's own span: 66 dB over does not mean the
// whole scale is lost twice.
float noiseFloorLossFraction(float overDb);

// Draws `channels` bars at `pos`. `node` may be null — a track with no live
// graph node meters as silence rather than disappearing, so the row keeps its
// shape. Clicking clears the latched clip indicator.
//
// Leaves the ImGui cursor where it found it: callers position this absolutely
// inside layouts they own.
// Clicking opens a menu that edits `options` in place, so every meter sharing
// that instance changes together. Returns true when the user changed
// something, so the caller can persist it.
// `floatHeadroom` says the session renders 32-bit float, which is the only
// case where going above 0 dBFS is worth showing as headroom rather than as a
// clip. Fixed-point sessions keep the old red.
// `allowPlacementDrag` makes a vertical drag on the meter flip
// options.belowFader — dragging down moves it below the fader, up returns it.
// Only the channel strip passes true; the track-header and mixer meters keep a
// plain click-for-menu. A clean click still opens the menu either way.
bool drawLevelMeter(engine::GainNode* node, ImVec2 pos, float height,
                    LevelMeterOptions& options, int channels = 2,
                    const LevelMeterStyle& style = LevelMeterStyle{},
                    bool floatHeadroom = false,
                    bool allowPlacementDrag = false);

} // namespace dave::gui
