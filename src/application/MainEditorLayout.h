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
    // The track list, on the left. Zero when it is closed — including its
    // splitter, so a closed panel costs nothing at all.
    float leftPanelWidth = 0.0f;
    float timelineHeight = 0.0f;
    float mixerHeight = 0.0f;
};

inline MainEditorLayout calculateMainEditorLayout(
    float windowWidth, float contentHeight, float splitterSize,
    float requestedSidebarWidth, float requestedMixerHeight, bool showMixer,
    float minimumEditorWidth = 320.0f,
    float minimumEditorHeight = 120.0f,
    bool showSidebar = true,
    float requestedLeftPanelWidth = 0.0f,
    bool showLeftPanel = false) noexcept {
    // A hidden sidebar costs nothing, splitter included: the arrangement
    // editor takes the whole width rather than leaving a six-pixel gutter
    // against a panel that isn't there.
    const float splitters = (showSidebar ? splitterSize : 0.0f) +
                            (showLeftPanel ? splitterSize : 0.0f);
    const float availableWidth = std::max(0.0f, windowWidth - splitters);

    // The left panel is sized first: it is a list at a readable width rather
    // than a proportion, and the arrangement editor is what absorbs whatever
    // is left. Both panels together still cannot squeeze the editor below its
    // minimum — the last one clamped gives way.
    const float leftPanelWidth =
        showLeftPanel
            ? std::clamp(requestedLeftPanelWidth, 0.0f,
                         std::max(0.0f, availableWidth - minimumEditorWidth))
            : 0.0f;
    const float maximumSidebarWidth =
        std::max(0.0f, availableWidth - leftPanelWidth - minimumEditorWidth);
    const float sidebarWidth =
        showSidebar ? std::clamp(requestedSidebarWidth, 0.0f, maximumSidebarWidth)
                    : 0.0f;

    float mixerHeight = 0.0f;
    if (showMixer) {
        const float maximumMixerHeight = std::max(
            0.0f, contentHeight - splitterSize - minimumEditorHeight);
        mixerHeight =
            std::clamp(requestedMixerHeight, 0.0f, maximumMixerHeight);
    }

    return {
        availableWidth - sidebarWidth - leftPanelWidth,
        sidebarWidth,
        leftPanelWidth,
        std::max(0.0f, contentHeight - mixerHeight -
                           (showMixer ? splitterSize : 0.0f)),
        mixerHeight,
    };
}

} // namespace dave::application
