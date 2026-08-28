// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip fades: the F-key gesture that turns a timeline selection into a fade,
// and the round trip through the project file. The gesture is the part with
// judgement in it — a range at a clip's head is a fade-in, at its tail a
// fade-out, an interior range fades from the nearer edge, and a whole-clip
// selection top-and-tails with a default. That mapping is what these pin.
#include "document/Edit.h"
#include "document/Fade.h"
#include "document/ProjectFile.h"
#include "editing/Command.h"
#include "editing/Commands.h"
#include "gui/Timeline.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dave;

namespace {

using document::FadeShape;

struct FadeRig {
    document::Edit edit;
    editing::UndoStack undo{edit};
    gui::TimelineViewState view;
    std::string track;
    std::string clip;

    FadeRig() {
        track = edit.addTrack("Audio");
        document::AudioClip c;
        c.timelineStart = 1000;
        c.length = 10000;  // clip spans [1000, 11000)
        clip = edit.addClip(track, c);
    }

    void select(int64_t start, int64_t end, int row = 0) {
        view.hasSelection = true;
        view.selectionStart = start;
        view.selectionEnd = end;
        view.selectionRow = row;
    }

    bool fade(FadeShape in = FadeShape::EqualPower,
              FadeShape out = FadeShape::Slow, int64_t defaultLen = 240) {
        return gui::createFadeFromSelection(edit, undo, view, in, out,
                                            defaultLen);
    }

    const document::AudioClip& c() const {
        return edit.track(track)->clips.front();
    }
};

}  // namespace

TEST_CASE("a selection at a clip's head becomes a fade-in", "[clipfades]") {
    FadeRig rig;
    // Reaches past the clip's start (1000) to sample 3000 inside it.
    rig.select(500, 3000);
    REQUIRE(rig.fade());
    // The fade grows from the head, so its length is where the selection ends.
    CHECK(rig.c().fadeIn == 2000);
    CHECK(rig.c().fadeInShape == FadeShape::EqualPower);
    // The tail is left alone.
    CHECK(rig.c().fadeOut == 0);
}

TEST_CASE("a selection at a clip's tail becomes a fade-out", "[clipfades]") {
    FadeRig rig;
    // Reaches from 9000 past the clip's end (11000).
    rig.select(9000, 12000);
    REQUIRE(rig.fade());
    CHECK(rig.c().fadeOut == 2000);  // 11000 - 9000
    CHECK(rig.c().fadeOutShape == FadeShape::Slow);
    CHECK(rig.c().fadeIn == 0);
}

TEST_CASE("an interior selection fades from the nearer edge", "[clipfades]") {
    SECTION("nearer the head") {
        FadeRig rig;
        rig.select(1500, 3000);  // 500 from head, 8000 from tail
        REQUIRE(rig.fade());
        CHECK(rig.c().fadeIn == 2000);  // head -> selection end
        CHECK(rig.c().fadeOut == 0);
    }
    SECTION("nearer the tail") {
        FadeRig rig;
        rig.select(8000, 9500);  // 7000 from head, 1500 from tail
        REQUIRE(rig.fade());
        CHECK(rig.c().fadeOut == 3000);  // selection start -> tail (11000-8000)
        CHECK(rig.c().fadeIn == 0);
    }
}

TEST_CASE("selecting a whole clip top-and-tails with the default length",
          "[clipfades]") {
    FadeRig rig;
    // Exactly the clip extent — what a single click on the clip leaves behind.
    rig.select(1000, 11000);
    REQUIRE(rig.fade(FadeShape::SCurve, FadeShape::SCurve, 240));
    CHECK(rig.c().fadeIn == 240);
    CHECK(rig.c().fadeOut == 240);
    CHECK(rig.c().fadeInShape == FadeShape::SCurve);
    CHECK(rig.c().fadeOutShape == FadeShape::SCurve);
}

TEST_CASE("a fade never overruns the other end of the clip", "[clipfades]") {
    FadeRig rig;
    // Pre-existing long fade-out via the command directly.
    rig.undo.execute(std::make_unique<editing::SetClipFadeCommand>(
        rig.track, rig.clip, 0, FadeShape::Linear, 8000, FadeShape::Linear));
    REQUIRE(rig.c().fadeOut == 8000);
    // Now a head selection wanting a 5500-sample fade-in: 5500 + 8000 > 10000,
    // so the fade-out is pulled back to fit rather than the two overlapping.
    rig.select(500, 6500);
    REQUIRE(rig.fade());
    CHECK(rig.c().fadeIn == 5500);
    CHECK(rig.c().fadeOut == 10000 - 5500);
}

TEST_CASE("fades over several clips are one undo step", "[clipfades]") {
    FadeRig rig;
    // A second clip on the same row, further along.
    document::AudioClip c2;
    c2.timelineStart = 20000;
    c2.length = 10000;
    const std::string clip2 = rig.edit.addClip(rig.track, c2);
    // A range reaching the head of both clips.
    rig.select(500, 30000);  // spans into clip2's interior too
    const size_t before = rig.edit.track(rig.track)->clips.size();
    REQUIRE(rig.fade());
    (void)before;
    // Both clips faded...
    CHECK(rig.edit.track(rig.track)->clips[0].fadeIn > 0);
    CHECK(rig.edit.track(rig.track)->clips[1].fadeIn > 0);
    // ...and a single undo clears both, proving they were one compound step.
    rig.undo.undo();
    CHECK(rig.edit.track(rig.track)->clips[0].fadeIn == 0);
    CHECK(rig.edit.track(rig.track)->clips[1].fadeIn == 0);
    CHECK_FALSE(rig.undo.canUndo());
}

TEST_CASE("undo restores the fade that was there before", "[clipfades]") {
    FadeRig rig;
    rig.select(500, 3000);
    REQUIRE(rig.fade());
    REQUIRE(rig.c().fadeIn == 2000);
    rig.undo.undo();
    CHECK(rig.c().fadeIn == 0);
}

TEST_CASE("F does nothing without a range or over MIDI only", "[clipfades]") {
    SECTION("no selection") {
        FadeRig rig;
        CHECK_FALSE(rig.fade());
    }
    SECTION("a MIDI-only row is skipped") {
        document::Edit edit;
        editing::UndoStack undo{edit};
        gui::TimelineViewState view;
        const std::string t = edit.addTrack("Inst");
        document::MidiClip m;
        m.timelineStart = 1000;
        m.length = 10000;
        edit.addMidiClip(t, m);
        view.hasSelection = true;
        view.selectionStart = 500;
        view.selectionEnd = 3000;
        view.selectionRow = 0;
        CHECK_FALSE(gui::createFadeFromSelection(
            edit, undo, view, FadeShape::Linear, FadeShape::Linear, 240));
    }
}

TEST_CASE("fade lengths and shapes survive a project round trip",
          "[clipfades][persistence]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    document::AudioClip c;
    c.timelineStart = 0;
    c.length = 48000;
    const std::string id = edit.addClip(t, c);
    auto* clip = edit.clip(t, id);
    REQUIRE(clip != nullptr);
    clip->fadeIn = 1234;
    clip->fadeOut = 5678;
    clip->fadeInShape = FadeShape::SCurve;
    clip->fadeOutShape = FadeShape::EqualPower;

    const std::string json = document::serializeEdit(edit);
    document::Edit loaded;
    REQUIRE(document::deserializeEdit(json, loaded).ok);

    const auto& rc = loaded.track(t)->clips.front();
    CHECK(rc.fadeIn == 1234);
    CHECK(rc.fadeOut == 5678);
    CHECK(rc.fadeInShape == FadeShape::SCurve);
    CHECK(rc.fadeOutShape == FadeShape::EqualPower);
}

TEST_CASE("A and S trim the selected clip to the selection", "[clipedit]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    gui::TimelineViewState view;
    const std::string t = edit.addTrack("Audio");
    document::AudioClip c;
    c.timelineStart = 1000;
    c.length = 10000;      // clip [1000, 11000)
    c.sourceOffset = 500;
    const std::string id = edit.addClip(t, c);
    const int row = static_cast<int>(edit.tracks().size()) - 2;
    REQUIRE(edit.tracks()[static_cast<size_t>(row)].id == t);
    view.selectedTrackIndex = row;
    view.selectedClipId = id;
    view.hasSelection = true;
    view.selectionStart = 4000;
    view.selectionEnd = 8000;

    SECTION("A drops the head before the selection start") {
        REQUIRE(gui::trimSelectedClipToSelection(edit, undo, view, true));
        const auto& r = edit.track(t)->clips.front();
        CHECK(r.timelineStart == 4000);
        CHECK(r.sourceOffset == 3500);   // 500 + (4000 - 1000)
        CHECK(r.length == 7000);         // 10000 - 3000
    }
    SECTION("S drops the tail after the selection end") {
        REQUIRE(gui::trimSelectedClipToSelection(edit, undo, view, false));
        const auto& r = edit.track(t)->clips.front();
        CHECK(r.timelineStart == 1000);
        CHECK(r.sourceOffset == 500);
        CHECK(r.length == 7000);         // 8000 - 1000
    }
}

TEST_CASE("D and G fade the selected clip to the playhead", "[clipedit]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    gui::TimelineViewState view;
    const std::string t = edit.addTrack("Audio");
    document::AudioClip c;
    c.timelineStart = 1000;
    c.length = 10000;      // [1000, 11000)
    const std::string id = edit.addClip(t, c);
    view.selectedTrackIndex = static_cast<int>(edit.tracks().size()) - 2;
    view.selectedClipId = id;

    // D: fade in up to the playhead.
    REQUIRE(gui::fadeSelectedClipToPlayhead(edit, undo, view, 5000, true,
                                            document::FadeShape::Linear));
    CHECK(edit.track(t)->clips.front().fadeIn == 4000);   // 5000 - 1000
    // G: fade out from the playhead.
    REQUIRE(gui::fadeSelectedClipToPlayhead(edit, undo, view, 8000, false,
                                            document::FadeShape::Linear));
    CHECK(edit.track(t)->clips.front().fadeOut == 3000);  // 11000 - 8000

    // Playhead outside the clip does nothing.
    CHECK_FALSE(gui::fadeSelectedClipToPlayhead(edit, undo, view, 500, true,
                                                document::FadeShape::Linear));
}
