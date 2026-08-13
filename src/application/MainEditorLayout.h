// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace dave::application {

// Pixel-sized utility panes are controlled by their splitters. A native
// window resize spends all newly available space on the arrangement editor;
// it must not rewrite the user's sidebar or mixer sizes. The safety clamps
// only apply to the rendered frame when a window is too small to fit them.
struct MainEditorLayout {
    float editorWidth = 0.0f;
    float sidebarWidth = 0.0f;
    float timelineHeight = 0.0f;
    float mixerHeight = 0.0f;
};

inline MainEditorLayout calculateMainEditorLayout(
    float windowWidth, float contentHeight, float splitterSize,
    float requestedSidebarWidth, float requestedMixerHeight, bool showMixer,
    float minimumEditorWidth = 320.0f,
    float minimumEditorHeight = 120.0f) noexcept {
    const float availableWidth =
        std::max(0.0f, windowWidth - splitterSize);
    const float maximumSidebarWidth =
        std::max(0.0f, availableWidth - minimumEditorWidth);
    const float sidebarWidth =
        std::clamp(requestedSidebarWidth, 0.0f, maximumSidebarWidth);

    float mixerHeight = 0.0f;
    if (showMixer) {
        const float maximumMixerHeight = std::max(
            0.0f, contentHeight - splitterSize - minimumEditorHeight);
        mixerHeight =
            std::clamp(requestedMixerHeight, 0.0f, maximumMixerHeight);
    }

    return {
        availableWidth - sidebarWidth,
        sidebarWidth,
        std::max(0.0f, contentHeight - mixerHeight -
                           (showMixer ? splitterSize : 0.0f)),
        mixerHeight,
    };
}

} // namespace dave::application
