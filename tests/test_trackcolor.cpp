// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImGuiTestRig.h"

#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "gui/Theme.h"
#include "gui/TrackColorPicker.h"

#include <catch2/catch_test_macros.hpp>

namespace {
// Main is a row in the one track list now, so a test asking "how many tracks
// did I make?" has to say so. Counting user rows keeps the intent visible
// rather than burying a +1 in every expectation.
inline size_t userTracks(const dave::document::Edit& e) {
    size_t n = 0;
    for (const auto& t : e.tracks()) if (!t.isMain) ++n;
    return n;
}
} // namespace

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
    CHECK(loaded.track(midi)->color == "#68a0aa");
    CHECK(loaded.track(bus)->color == "#8d79a8");
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

TEST_CASE("header text contrasts with whatever colour the track has",
          "[trackcolor]") {
    // The picker offers light and dark colours, so a fixed foreground would be
    // unreadable on half of what the user can choose.
    const ImVec4 lightGreen(0.72f, 0.86f, 0.70f, 1.0f);
    const ImVec4 deepBlue(0.10f, 0.14f, 0.32f, 1.0f);

    const ImVec4 onLight = dave::gui::theme::readableTextOn(lightGreen);
    const ImVec4 onDark = dave::gui::theme::readableTextOn(deepBlue);
    CHECK(onLight.x < 0.5f);    // dark ink on a light field
    CHECK(onDark.x > 0.5f);     // light ink on a dark one

    // Perceived luminance, not a plain average: a saturated blue and a
    // saturated green of the same mean brightness look nothing alike, and
    // averaging would put this pair on the same side of the decision.
    const ImVec4 midGreen(0.0f, 0.75f, 0.0f, 1.0f);
    const ImVec4 midBlue(0.0f, 0.0f, 0.75f, 1.0f);
    CHECK((midGreen.x + midGreen.y + midGreen.z) ==
          (midBlue.x + midBlue.y + midBlue.z));
    CHECK(dave::gui::theme::readableTextOn(midGreen).x < 0.5f);
    CHECK(dave::gui::theme::readableTextOn(midBlue).x > 0.5f);
}

TEST_CASE("an uncoloured track has one colour, not one per view",
          "[trackcolor]") {
    // The timeline band and the channel strip header both answer "what colour
    // is this track". They used to answer separately — the timeline cycled a
    // palette by row, the strip fell back to the theme accent — so an
    // uncoloured track was one colour on the timeline and another in the
    // strip that claimed to belong to it.
    using dave::gui::defaultTrackColor;
    using dave::gui::trackColorValue;
    // applyTheme writes into ImGuiStyle, so it needs a context. Without the
    // theme the palette is all zeroes and "adjacent rows differ" would fail
    // for a reason that has nothing to do with the cycle.
    dave::testing::ImGuiRig rig;
    dave::gui::theme::applyTheme();

    // The cycle repeats every six rows and never reads off the front.
    for (int row = 0; row < 6; ++row) {
        const ImVec4 a = defaultTrackColor(row);
        const ImVec4 b = defaultTrackColor(row + 6);
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
        CHECK(a.z == b.z);
    }
    const ImVec4 negative = defaultTrackColor(-1);
    const ImVec4 wrapped = defaultTrackColor(5);
    CHECK(negative.x == wrapped.x);
    CHECK(negative.y == wrapped.y);

    // Adjacent rows differ, or the cycle is not doing anything.
    const bool adjacentMatch =
        defaultTrackColor(0).x == defaultTrackColor(1).x &&
        defaultTrackColor(0).y == defaultTrackColor(1).y &&
        defaultTrackColor(0).z == defaultTrackColor(1).z;
    CHECK_FALSE(adjacentMatch);

    // An explicit colour still wins over the row's default in both views.
    const ImVec4 chosen = trackColorValue("#ff8800", defaultTrackColor(3));
    CHECK(chosen.x > 0.9f);
    CHECK(chosen.z < 0.1f);
}
