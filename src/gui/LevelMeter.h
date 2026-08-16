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

// Draws `channels` bars at `pos`. `node` may be null — a track with no live
// graph node meters as silence rather than disappearing, so the row keeps its
// shape. Clicking clears the latched clip indicator.
//
// Leaves the ImGui cursor where it found it: callers position this absolutely
// inside layouts they own.
// Clicking opens a menu that edits `options` in place, so every meter sharing
// that instance changes together. Returns true when the user changed
// something, so the caller can persist it.
bool drawLevelMeter(engine::GainNode* node, ImVec2 pos, float height,
                    LevelMeterOptions& options, int channels = 2,
                    const LevelMeterStyle& style = LevelMeterStyle{});

} // namespace dave::gui
