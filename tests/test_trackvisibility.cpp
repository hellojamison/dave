// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hiding a track.
//
// This is a view state stored in the document, not a mute: a hidden track goes
// on playing, recording and receiving sends exactly as before. What it must
// not do is renumber anything — every selection, drag and command in the
// timeline indexes tracks by their position in the document.
#include "ImGuiTestRig.h"

#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "engine/GraphBuilder.h"
#include "gui/Timeline.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace dave;

TEST_CASE("hiding a track does not move the others", "[trackvisibility]") {
    // The failure this guards: compacting hidden rows out of the layout would
    // silently renumber every track index the moment one was hidden, so a
    // selection made before the hide would point at a different track after.
    document::Edit edit;
    const std::string a = edit.addTrack("A");
    const std::string b = edit.addTrack("B");
    const std::string c = edit.addTrack("C");

    REQUIRE(edit.setTrackHidden(b, true));
    const auto& tracks = edit.tracks();
    REQUIRE(tracks.size() == 4);   // three plus Main
    CHECK(tracks[0].id == a);
    CHECK(tracks[1].id == b);
    CHECK(tracks[2].id == c);
    CHECK(tracks[1].hidden);
    CHECK_FALSE(tracks[0].hidden);
}

TEST_CASE("Main cannot be hidden", "[trackvisibility]") {
    // It is where everything ends up, and there would be no visible output.
    document::Edit edit;
    CHECK_FALSE(edit.setTrackHidden(std::string(document::kMainBusId), true));
    CHECK_FALSE(edit.track(std::string(document::kMainBusId))->hidden);
}

TEST_CASE("hiding is undoable", "[trackvisibility]") {
    // Easy to hit by accident, and the track is then not on screen to put
    // back by hand.
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string a = edit.addTrack("A");

    undo.execute(std::make_unique<editing::SetTrackHiddenCommand>(a, true));
    CHECK(edit.track(a)->hidden);
    undo.undo();
    CHECK_FALSE(edit.track(a)->hidden);
    undo.redo();
    CHECK(edit.track(a)->hidden);
}

TEST_CASE("showing all is one undo, not one per track",
          "[trackvisibility]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string a = edit.addTrack("A");
    const std::string b = edit.addTrack("B");
    REQUIRE(edit.setTrackHidden(a, true));
    REQUIRE(edit.setTrackHidden(b, true));

    undo.execute(std::make_unique<editing::ShowAllTracksCommand>());
    CHECK_FALSE(edit.track(a)->hidden);
    CHECK_FALSE(edit.track(b)->hidden);

    undo.undo();
    CHECK(edit.track(a)->hidden);
    CHECK(edit.track(b)->hidden);
}

TEST_CASE("a hidden track still plays", "[trackvisibility]") {
    // Hiding is not muting. A hidden track keeps its node in the graph, or
    // this would be a mute with a confusing name.
    document::Edit edit;
    const std::string a = edit.addTrack("A");
    REQUIRE(edit.setTrackHidden(a, true));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 2);
    REQUIRE(graph != nullptr);
    CHECK(builder.trackGains().count(a) == 1);
    CHECK(builder.meterTaps().count(a) == 1);
}

TEST_CASE("hidden survives a save", "[trackvisibility]") {
    document::Edit edit;
    const std::string a = edit.addTrack("A");
    edit.addTrack("B");
    REQUIRE(edit.setTrackHidden(a, true));

    const std::string text = document::serializeEdit(edit);
    document::Edit reloaded;
    REQUIRE(document::deserializeEdit(text, reloaded).ok);
    REQUIRE(reloaded.track(a) != nullptr);
    CHECK(reloaded.track(a)->hidden);
    CHECK_FALSE(reloaded.tracks()[1].hidden);
}

TEST_CASE("a hidden track takes no room and cannot be hit",
          "[trackvisibility]") {
    // Through the real widget: a zero-height row means the track below it
    // moves up into the space, and a click there lands on that track rather
    // than on the hidden one.
    struct Rig : dave::testing::ImGuiRig {
        void tick(float x, float y, bool down) {
            frame(x, y, down, [&] {
                gui::drawTimeline(edit, undo, transport, peaks, view,
                                  assetBuffers, 58.0f);
            });
            origin = ImVec2(0.0f, 0.0f);
        }
        gui::PeakCache peaks;
        std::unordered_map<std::string, audio::DecodedAudioAssetPtr>
            assetBuffers;
        ImVec2 origin{};
    };

    Rig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    const std::string cb = rig.edit.addClip(b, clip);

    // With A visible, B's clip is on the second row.
    rig.tick(-100.0f, -100.0f, false);
    auto findClipRow = [&]() {
        for (float y = 40.0f; y < 400.0f; y += 2.0f) {
            rig.view.selectedClipId.clear();
            rig.tick(400.0f, y, false);
            rig.tick(400.0f, y, true);
            const std::string hit = rig.view.selectedClipId;
            rig.tick(400.0f, y, false);
            if (hit == cb) return y;
        }
        return -1.0f;
    };
    const float before = findClipRow();
    REQUIRE(before > 0.0f);

    REQUIRE(rig.edit.setTrackHidden(a, true));
    const float after = findClipRow();
    REQUIRE(after > 0.0f);
    // B moved up into the space A was taking.
    CHECK(after < before);
}

// ─── Solo safe ─────────────────────────────────────────────────────────────
//
// A track exempt from other tracks' solos: a talkback, a click, a reverb
// return you always want to hear. Soloing a guitar should not silence the
// reverb that guitar is feeding.

TEST_CASE("solo safe survives another track's solo", "[trackvisibility]") {
    CHECK(document::trackAudible(false, false, true, false) == false);
    // The whole feature in one line.
    CHECK(document::trackAudible(false, false, true, true));
    // With nothing soloed it changes nothing.
    CHECK(document::trackAudible(false, false, false, false));
}

TEST_CASE("mute still beats solo safe", "[trackvisibility]") {
    // Solo safe means "another track's solo does not silence me", not "I
    // cannot be switched off" — a safe track you muted on purpose stays off.
    CHECK_FALSE(document::trackAudible(true, false, true, true));
    CHECK_FALSE(document::trackAudible(true, true, true, true));
}

TEST_CASE("a solo-safe track keeps its output when another is soloed",
          "[trackvisibility]") {
    // The gain is only half of it: with any solo active the graph drops every
    // edge that is not on a soloed path, so a safe track would be exempt from
    // the muting and still disconnected from Main.
    document::Edit edit;
    const std::string safe = edit.addTrack("Return");
    const std::string other = edit.addTrack("Guitar");
    REQUIRE(edit.setTrackSoloSafe(safe, true));
    edit.track(other)->solo = true;
    edit.track(safe)->gain = 1.0;
    REQUIRE(edit.setInputMonitor(safe, true));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 2);
    REQUIRE(graph != nullptr);
    REQUIRE(builder.trackGains().count(safe) == 1);
    // Not zeroed by the other track's solo.
    CHECK(builder.trackGains().at(safe)->gain() > 0.0);
}

TEST_CASE("solo safe is undoable and persists", "[trackvisibility]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Return");

    undo.execute(std::make_unique<editing::SetTrackSoloSafeCommand>(t, true));
    CHECK(edit.track(t)->soloSafe);
    undo.undo();
    CHECK_FALSE(edit.track(t)->soloSafe);
    undo.redo();
    CHECK(edit.track(t)->soloSafe);

    const std::string text = document::serializeEdit(edit);
    document::Edit reloaded;
    REQUIRE(document::deserializeEdit(text, reloaded).ok);
    CHECK(reloaded.track(t)->soloSafe);
}
