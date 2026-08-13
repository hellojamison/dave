// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "gui/TrackColorPicker.h"

#include <catch2/catch_test_macros.hpp>

using namespace dave;

TEST_CASE("track colors validate and round-trip for every channel type",
          "[track-color][document]") {
    document::Edit edit;
    const auto audio = edit.addTrack("DX");
    const auto midi = edit.addMidiTrack("Keys");
    const auto bus = edit.addBus("Stem");

    REQUIRE(edit.setTrackColor(audio, "#c96f72"));
    REQUIRE(edit.setTrackColor(midi, "#68a0aa"));
    REQUIRE(edit.setTrackColor(bus, "#8d79a8"));
    CHECK_FALSE(edit.setTrackColor(audio, "red"));

    document::Edit loaded;
    REQUIRE(document::deserializeEdit(document::serializeEdit(edit), loaded).ok);
    CHECK(loaded.track(audio)->color == "#c96f72");
    CHECK(loaded.midiTrack(midi)->color == "#68a0aa");
    CHECK(loaded.bus(bus)->color == "#8d79a8");
}

TEST_CASE("track color changes undo and restore the type default",
          "[track-color][commands]") {
    document::Edit edit;
    editing::UndoStack undo(edit);
    const auto track = edit.addTrack("FX");

    undo.execute(std::make_unique<editing::SetTrackColorCommand>(
        track, "#79a076"));
    CHECK(edit.track(track)->color == "#79a076");
    undo.undo();
    CHECK(edit.track(track)->color.empty());
    undo.redo();
    CHECK(edit.track(track)->color == "#79a076");

    undo.execute(std::make_unique<editing::SetTrackColorCommand>(track, ""));
    CHECK(edit.track(track)->color.empty());
}

TEST_CASE("track palette exposes distinct named swatches",
          "[track-color][ui]") {
    const auto& choices = gui::trackColorChoices();
    CHECK(choices.size() == 20);
    for (const auto& choice : choices) {
        CHECK(choice.name[0] != '\0');
        CHECK(document::validTrackColor(choice.hex));
    }

    const ImVec4 fallback(0.1f, 0.2f, 0.3f, 1.0f);
    const auto parsed = gui::trackColorValue("#ff8000", fallback);
    CHECK(parsed.x == 1.0f);
    CHECK(parsed.y > 0.49f);
    CHECK(parsed.z == 0.0f);
    const auto defaulted = gui::trackColorValue("", fallback);
    CHECK(defaulted.x == fallback.x);
    CHECK(defaulted.y == fallback.y);
    CHECK(defaulted.z == fallback.z);
}
