// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands after the three track types were merged into one.
//
// Two things are under test here. The first is that the unified add/remove
// track commands survive undo-redo with the row's identity and position
// intact — an id that changes on redo is invisible until something keyed by
// it (a graph node, a selection, a send) silently points at nothing.
//
// The second is the set of defects the collapse inherited away. The audio
// clip commands were the broken siblings; the MIDI ones were written later
// and written correctly. Merging meant adopting the MIDI implementation, so
// each case below is a bug that was reachable in the shipped audio command
// and is not reachable in its MIDI counterpart.
#include "document/Edit.h"
#include "editing/Command.h"
#include "editing/Commands.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace dave;

namespace {

// Main is a row in the one track list now, so a test asking "which track did
// I make?" has to say so.
std::vector<const document::Track*> userTracks(const document::Edit& e) {
    std::vector<const document::Track*> out;
    for (const auto& t : e.tracks()) if (!t.isMain) out.push_back(&t);
    return out;
}

document::AudioClip audioClip(int64_t start, int64_t length) {
    document::AudioClip c;
    c.timelineStart = start;
    c.length = length;
    return c;
}

} // namespace

// ─── One add-track command ─────────────────────────────────────────────────

TEST_CASE("every track flavour keeps its id across undo and redo",
          "[trackcommands]") {
    using Flavour = editing::AddTrackCommand::Flavour;
    const Flavour flavours[] = {Flavour::Audio, Flavour::Midi, Flavour::Bus};

    for (Flavour flavour : flavours) {
        document::Edit edit;
        editing::UndoStack undo{edit};

        auto cmd = std::make_unique<editing::AddTrackCommand>("Row", flavour);
        auto* raw = cmd.get();
        undo.execute(std::move(cmd));

        const std::string first = raw->trackId();
        REQUIRE_FALSE(first.empty());
        REQUIRE(edit.track(first) != nullptr);

        undo.undo();
        REQUIRE(edit.track(first) == nullptr);

        undo.redo();
        // The pre-merge AddTrackCommand assigned `trackId_ = e.addTrack(...)`
        // unconditionally, so redo minted a second id and left every reference
        // to the first one dangling. Only the MIDI and bus commands patched it.
        CHECK(raw->trackId() == first);
        CHECK(edit.track(first) != nullptr);
        CHECK(userTracks(edit).size() == 1);
    }
}

TEST_CASE("a new track lands ahead of Main whatever made it",
          "[trackcommands]") {
    using Flavour = editing::AddTrackCommand::Flavour;
    document::Edit edit;
    editing::UndoStack undo{edit};

    undo.execute(std::make_unique<editing::AddTrackCommand>("Audio"));
    undo.execute(std::make_unique<editing::AddTrackCommand>("Keys",
                                                            Flavour::Midi));
    undo.execute(std::make_unique<editing::AddTrackCommand>("Reverb",
                                                            Flavour::Bus));

    REQUIRE(edit.tracks().size() == 4);
    CHECK(edit.tracks().back().isMain);
    const auto rows = userTracks(edit);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0]->name == "Audio");
    CHECK(rows[1]->name == "Keys");
    CHECK(rows[2]->name == "Reverb");
}

// ─── One remove-track command ──────────────────────────────────────────────

TEST_CASE("removing an audio track can be undone whole", "[trackcommands]") {
    // Audio tracks had no remove command at all before the merge — the UI
    // called Edit::removeTrack directly, so deleting one was not undoable.
    document::Edit edit;
    editing::UndoStack undo{edit};

    const std::string a = edit.addTrack("A");
    const std::string b = edit.addTrack("B");
    const std::string c = edit.addTrack("C");
    edit.addClip(b, audioClip(48000, 96000));
    edit.setTrackColor(b, "#ff8800");

    undo.execute(std::make_unique<editing::RemoveTrackCommand>(b));
    REQUIRE(edit.track(b) == nullptr);
    REQUIRE(userTracks(edit).size() == 2);

    undo.undo();

    const auto rows = userTracks(edit);
    REQUIRE(rows.size() == 3);
    // Row order is what the user sees, so restoring the track at the end
    // would be a visible corruption even though nothing was lost.
    CHECK(rows[0]->id == a);
    CHECK(rows[1]->id == b);
    CHECK(rows[2]->id == c);
    REQUIRE(rows[1]->clips.size() == 1);
    CHECK(rows[1]->clips.front().timelineStart == 48000);
    CHECK(rows[1]->color == "#ff8800");
    CHECK(edit.tracks().back().isMain);
}

TEST_CASE("removing a track referenced by a send is refused whatever made it",
          "[trackcommands]") {
    using Flavour = editing::AddTrackCommand::Flavour;
    document::Edit edit;
    editing::UndoStack undo{edit};

    const std::string source = edit.addTrack("Source");
    auto cmd = std::make_unique<editing::AddTrackCommand>("Keys",
                                                          Flavour::Midi);
    auto* raw = cmd.get();
    undo.execute(std::move(cmd));
    const std::string target = raw->trackId();

    document::AuxSend send;
    send.target = document::RouteTarget::bus(target);
    REQUIRE_FALSE(edit.addSend(source, send).empty());

    undo.execute(std::make_unique<editing::RemoveTrackCommand>(target));
    // Edit::removeMidiTrack had no routeReferences guard, so this used to
    // leave the send pointing at a track that no longer existed.
    CHECK(edit.track(target) != nullptr);
}

// ─── The clip commands the merge repaired ──────────────────────────────────

TEST_CASE("an added audio clip keeps its id across redo",
          "[trackcommands][clipcommands]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");

    auto cmd = std::make_unique<editing::AddClipCommand>(t, audioClip(0, 48000));
    auto* raw = cmd.get();
    undo.execute(std::move(cmd));
    const std::string first = raw->clipId();
    REQUIRE_FALSE(first.empty());

    undo.undo();
    undo.redo();

    // AddClipCommand re-minted the id on every redo; AddMidiClipCommand
    // patched the original back. This is the MIDI behaviour.
    CHECK(raw->clipId() == first);
    REQUIRE(edit.track(t)->clips.size() == 1);
    CHECK(edit.track(t)->clips.front().id == first);
    CHECK(edit.clip(t, first) != nullptr);
}

TEST_CASE("undoing a cross-track move restores the original position",
          "[trackcommands][clipcommands]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string from = edit.addTrack("From");
    const std::string to = edit.addTrack("To");
    const std::string clip = edit.addClip(from, audioClip(48000, 96000));

    undo.execute(std::make_unique<editing::MoveClipCommand>(
        from, clip, 240000, to));
    REQUIRE(edit.track(from)->clips.empty());
    REQUIRE(edit.track(to)->clips.size() == 1);
    CHECK(edit.clip(to, clip)->timelineStart == 240000);

    undo.undo();

    REQUIRE(edit.track(to)->clips.empty());
    REQUIRE(edit.track(from)->clips.size() == 1);
    // The old undo re-added the snapshot without resetting its timelineStart,
    // which perform() had already overwritten with the destination position.
    // The clip came back on the right track at the wrong place.
    CHECK(edit.clip(from, clip)->timelineStart == 48000);
    CHECK(edit.clip(from, clip)->length == 96000);
}

TEST_CASE("a split outside the clip is refused",
          "[trackcommands][clipcommands]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    const std::string clip = edit.addClip(t, audioClip(0, 48000));

    // Right-clicking a clip with the playhead parked past its end reached
    // this. The old command computed both halves by subtraction without
    // checking, giving the right half a negative length and making the left
    // half longer than the original.
    undo.execute(std::make_unique<editing::SplitClipCommand>(t, clip, 96000));
    REQUIRE(edit.track(t)->clips.size() == 1);
    CHECK(edit.clip(t, clip)->length == 48000);

    undo.execute(std::make_unique<editing::SplitClipCommand>(t, clip, -1000));
    REQUIRE(edit.track(t)->clips.size() == 1);
    CHECK(edit.clip(t, clip)->length == 48000);
}

TEST_CASE("splitting an audio clip is exactly reversible and replayable",
          "[trackcommands][clipcommands]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    const std::string clip = edit.addClip(t, audioClip(0, 48000));

    auto cmd = std::make_unique<editing::SplitClipCommand>(t, clip, 12000);
    auto* raw = cmd.get();
    undo.execute(std::move(cmd));

    REQUIRE(edit.track(t)->clips.size() == 2);
    const std::string right = raw->rightId();
    REQUIRE_FALSE(right.empty());
    CHECK(edit.clip(t, clip)->length == 12000);
    CHECK(edit.clip(t, right)->length == 36000);
    CHECK(edit.clip(t, right)->timelineStart == 12000);
    CHECK(edit.clip(t, right)->sourceOffset == 12000);

    undo.undo();
    REQUIRE(edit.track(t)->clips.size() == 1);
    // Restored from a snapshotted length, not rebuilt by adding the halves
    // back together — the reconstruction depended on state the command does
    // not own.
    CHECK(edit.clip(t, clip)->length == 48000);

    undo.redo();
    REQUIRE(edit.track(t)->clips.size() == 2);
    // The old undo cleared rightId_, so every redo produced a different right
    // half and any reference to it went stale.
    CHECK(raw->rightId() == right);
    CHECK(edit.clip(t, right) != nullptr);
    CHECK(edit.clip(t, right)->length == 36000);
}
