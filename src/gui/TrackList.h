// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "gui/Timeline.h"

#include <imgui.h>

#include <cstddef>

namespace dave::gui {

// The track list: every track in the session, hidden ones included.
//
// Hiding a track from the timeline is only safe if there is somewhere it is
// still listed. The right-click menu can hide one and show them all, but
// neither tells you WHICH are hidden or brings back a single one — this is
// where that lives, and it is the reason the panel exists rather than a
// convenience on top of it.

// The eye's hit box inside a row, from the row's left edge. Exposed because a
// click has to land on it and a test should not have to guess where it is.
inline constexpr float kTrackListEyeWidth = 26.0f;
inline constexpr float kTrackListRowHeight = 22.0f;

// Which row a click at `y` falls on, or -1. Rows are uniform here — the list
// is a list, not a mirror of the timeline's variable row heights.
int trackListRowAt(float y, float listTop, size_t rowCount);

// The eye mark itself, so the toolbar toggle and the list rows draw the same
// thing. Two different icons for "visibility" would read as two features.
void drawTrackListEye(ImDrawList* dl, ImVec2 centre, bool open, ImU32 color);

// Draw the list. Toggling an eye writes a request into `view`; the
// application owns the undo stack and services it.
void drawTrackList(const document::Edit& edit, TimelineViewState& view);

} // namespace dave::gui
