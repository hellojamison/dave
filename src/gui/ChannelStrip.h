// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/plugins/PluginHost.h"
#include "gui/LevelMeter.h"
#include "gui/Timeline.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave::gui {

// The channel strip: one selected track, read top to bottom the way signal
// travels through it — input at the top, inserts in the middle, sends and the
// output at the bottom.
//
// It lives here rather than in DaveApp because the parts that can be got wrong
// are the drop-index arithmetic and the routing dispatch, and DaveApp is not in
// the test target. Everything the strip needs from the application it asks for
// through TimelineViewState's request fields, the same way the mixer does — so
// no callbacks and no host pointer.

// Where a row being dragged would land, given the actual top of every row in
// screen space. Returns an insertion slot, so a list of n rows has n valid
// answers: dragging below the last lands on it rather than off the end.
//
// Row heights are deliberately not assumed uniform. A send occupies two lines
// and an insert one, and assuming a single height silently multiplies how far
// a drag travels — which reads as the drag skipping rows rather than as a bug.
//
// `rowTops` must be ascending; `listBottom` closes the last row.
size_t dropIndexAmongRows(float mouseY, const std::vector<float>& rowTops,
                          float listBottom);

// Drag state for the sends list. Held in the view rather than in a static so
// two strips (or a strip and a test) can't share one drag.
struct ChannelStripState {
    // Which chain row is being dragged, or -1. One field for all of them:
    // inserts, sends, the meter and the fader are rows in one list, so a drag
    // is a drag whichever kind of row started it.
    int draggingChainRow = -1;
    int chainDropIndex = -1;

    // Typed into the insert/instrument pickers. Lives here rather than in a
    // static so two strips can't share one filter.
    char pluginFilter[64] = {};

    bool draggingChain() const { return draggingChainRow >= 0; }
    void clear() {
        draggingChainRow = -1;
        chainDropIndex = -1;
    }
};

// Draw the strip for whichever row the view has selected. `captureChannels` and
// `playbackChannels` are the live device widths; `locked` disables every
// topology control (recording is running).
void drawChannelStrip(document::Edit& edit,
                      editing::UndoStack& undo,
                      TimelineViewState& view,
                      ChannelStripState& strip,
                      int captureChannels,
                      int playbackChannels,
                      bool locked,
                      const std::vector<engine::PluginDescriptor>& plugins = {},
                      // Live tap nodes keyed by track id, from
                      // GraphBuilder::meterTaps(). Null meters as silence.
                      const std::unordered_map<
                          std::string, std::shared_ptr<engine::GainNode>>*
                          meterTaps = nullptr,
                      LevelMeterOptions* meterOptions = nullptr);

// Case-insensitive substring match, with an empty filter matching everything.
// Extracted because the pickers are popups and a popup is awkward to drive in
// a test, while the rule for what appears in one is not.
bool pluginMatchesFilter(const std::string& name, const char* filter);

// The document slot a descriptor becomes when chosen.
document::PluginSlot slotFromDescriptor(const engine::PluginDescriptor& d);

} // namespace dave::gui
