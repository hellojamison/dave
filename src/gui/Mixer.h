// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "editing/Command.h"
#include "engine/nodes/GainNode.h"
// The mixer shares the session's view state (selection, and the requests that
// have to be serviced by the application) with the timeline. Two view states
// would mean selecting a track in one place and not the other.
#include "gui/Timeline.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace dave::gui {

using TrackGainNodes =
    std::unordered_map<std::string, std::shared_ptr<engine::GainNode>>;

// Draw the mixer: one vertical strip per track, audio then MIDI, in the same
// order as the timeline rows.
//
// A strip is the track's signal path top to bottom — instrument (MIDI only),
// then the insert chain, then mute/solo, a pan knob and the fader. That order is the
// order the audio actually travels, which is the whole reason a mixer is laid
// out vertically.
//
// Inserts are the track's existing plugin chain; GraphBuilder has always
// routed audio through it as chainSource -> plugin[0] -> ... -> trackGain.
// What this adds is somewhere to see and edit that chain for every track at
// once, instead of one selected track at a time in the sidebar.
//
// Mutations go through `undo` where a command exists. Opening the plugin
// picker or a plugin's editor window can't happen from inside a draw call, so
// those are recorded in `view` for the application to service after the frame.
void drawMixer(document::Edit& edit,
               editing::UndoStack& undo,
               TimelineViewState& view,
               float stripWidth = 108.0f,
               int captureChannels = 0,
               int playbackChannels = 2,
               const TrackGainNodes* gainNodes = nullptr);

} // namespace dave::gui
