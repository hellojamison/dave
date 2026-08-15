// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the timeline puts its chrome, driven headlessly through the REAL
// drawTimeline.
//
// Two layout rules are worth guarding because both are invisible to any
// command-level test: the add-track control sits above the topmost track
// rather than below the last one, and the video lane does not exist at all
// until picture is imported. Both are pure geometry — a refactor can restore
// the old positions with every command still correct.
#include "ImGuiTestRig.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dave;
using dave::testing::ImGuiRig;

namespace {

constexpr float kTrackHeight = 58.0f;
constexpr float kGutterWidth = 260.0f;
constexpr float kRulerHeight = 30.0f;
constexpr float kMarkerLaneHeight = 28.0f;
constexpr float kAutomationLaneHeight = 72.0f;

float effectiveTrackHeight() {
    constexpr float rowPadding = 6.0f;
    constexpr float rowGap = 3.0f;
    const float compactControlHeight = ImGui::GetFontSize() + 2.0f;
    const float headerHeight = std::max(
        static_cast<float>(gui::theme::typeScale().label),
        compactControlHeight);
    return std::max(kTrackHeight,
                    rowPadding * 2.0f + headerHeight +
                        compactControlHeight * 2.0f + rowGap * 2.0f);
}

struct LayoutRig : ImGuiRig {
    LayoutRig() { view.samplesPerPixel = 500.0; }

    // drawTimeline draws from the host window's cursor, which sits inside the
    // window padding. Capturing it rather than assuming (0,0) keeps the probe
    // coordinates exact when the theme changes its padding.
    void tick(float x, float y, bool down) {
        frame(x, y, down, [&] {
            origin = ImGui::GetCursorScreenPos();
            gui::drawTimeline(edit, undo, transport, peaks, view, assetBuffers,
                              kTrackHeight);
        });
    }

    void clickTimelineAt(float x, float y) {
        tick(x, y, false);
        tick(x, y, true);
        tick(x, y, false);
    }

    void doubleClickTimelineAt(float x, float y) {
        // Let any prior click age out before enabling ImGui's double-click
        // window; otherwise a second call can begin as click three and close
        // the editor it just opened.
        for (int frame = 0; frame < 20; ++frame) {
            tick(-100.0f, -100.0f, false);
        }
        ImGui::GetIO().MouseDoubleClickTime = 0.30f;
        clickTimelineAt(x, y);
        clickTimelineAt(x, y);
        ImGui::GetIO().MouseDoubleClickTime = 0.0f;
    }

    void typeText(const char* text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        tick(-100.0f, -100.0f, false);
    }

    void pressKey(ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        tick(-100.0f, -100.0f, false);
        ImGui::GetIO().AddKeyEvent(key, false);
        tick(-100.0f, -100.0f, false);
    }

    // One frame parked away from every control, purely to learn `origin`.
    void settle() { tick(-100.0f, -100.0f, false); }

    // Transport::seek only requests a move; the RT thread commits it. Nothing
    // pumps audio here, so the test does that one block itself — otherwise
    // position() stays at 0 and a playhead assertion passes for the wrong
    // reason.
    void seekTo(int64_t sample) {
        transport.seek(sample);
        engine::TimeInfo time{};
        transport.advanceAndFill(time, 0, 48000.0);
    }

    // Modifier state is an event, processed at NewFrame, and it persists until
    // set back — so this holds Option down across a whole gesture.
    void holdAlt(bool down) { ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, down); }

    void holdSuper(bool down) {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Super, down);
    }

    ImVec2 origin{0.0f, 0.0f};
    gui::PeakCache peaks;
    std::unordered_map<std::string, audio::DecodedAudioAssetPtr> assetBuffers;
};

document::VideoClip videoClip() {
    document::VideoClip c;
    c.name = "Reel 1";
    c.timelineStart = 0;
    c.length = 480000;
    c.durationSeconds = 10.0;
    c.width = 1920;
    c.height = 1080;
    c.fps = 24.0;
    return c;
}

} // namespace

TEST_CASE("focused timeline routes Tab navigation into its view model",
          "[timelinelayout][transient]") {
    LayoutRig rig;
    rig.edit.addTrack("Dialogue");
    rig.settle();
    rig.pressKey(ImGuiKey_Tab);
    REQUIRE(rig.view.requestTransientNavigation);
    REQUIRE(rig.view.transientNavigationDirection ==
            gui::NavigationDirection::Next);
    REQUIRE_FALSE(rig.view.requestTransientSelectionExtension);

    rig.view.requestTransientNavigation = false;
#ifdef __APPLE__
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
#else
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
#endif
    rig.pressKey(ImGuiKey_Tab);
#ifdef __APPLE__
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
#else
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
#endif
    REQUIRE(rig.view.requestTransientNavigation);
    REQUIRE(rig.view.transientNavigationDirection ==
            gui::NavigationDirection::Previous);
}

TEST_CASE("the add-track + sits above the topmost track", "[timelinelayout]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    // The control is a 20 px square inset 10 px into the gutter, vertically
    // centred in the ruler row — i.e. above the first track header, not below
    // the last one.
    rig.clickTimelineAt(rig.origin.x + 20.0f,
                        rig.origin.y + kRulerHeight * 0.5f);

    REQUIRE(rig.edit.tracks().size() == 2);
    // And it goes through the undo stack rather than mutating the edit.
    CHECK(rig.undo.canUndo());
    rig.undo.undo();
    CHECK(rig.edit.tracks().size() == 1);
}

TEST_CASE("clicking below the last track no longer adds one",
          "[timelinelayout]") {
    // The add row used to span the full width immediately under the tracks.
    // Now that space is the drop canvas, and a stray click there must not
    // create a track.
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    const float belowTracks = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                              kTrackHeight + 20.0f;
    rig.clickTimelineAt(rig.origin.x + kGutterWidth + 200.0f, belowTracks);

    CHECK(rig.edit.tracks().size() == 1);
    CHECK_FALSE(rig.undo.canUndo());
}

TEST_CASE("the marker row's right-justified + adds a marker at the playhead",
          "[timelinelayout]") {
    LayoutRig rig;
    const std::string mt = rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.seekTo(96000);
    rig.settle();

    // An 18 px square inset 10 px from the gutter's right edge, vertically
    // centred in the 28 px marker lane below the ruler.
    rig.clickTimelineAt(rig.origin.x + kGutterWidth - 19.0f,
                        rig.origin.y + kRulerHeight + kMarkerLaneHeight * 0.5f);

    const auto* track = rig.edit.markerTrack(mt);
    REQUIRE(track != nullptr);
    REQUIRE(track->markers.size() == 1);
    // At the playhead, not at the pointer — the pointer was over the gutter.
    CHECK(track->markers.front().position == 96000);

    rig.undo.undo();
    CHECK(rig.edit.markerTrack(mt)->markers.empty());
}

TEST_CASE("the video lane consumes no height until picture is imported",
          "[timelinelayout]") {
    LayoutRig rig;
    float heightWithoutVideo = -1.0f;
    rig.frame(-100.0f, -100.0f, false, [&] {
        heightWithoutVideo = gui::drawVideoLane(
            rig.edit, rig.transport, rig.view,
            ImGui::GetCursorScreenPos(), 1200.0f, kGutterWidth,
            rig.view.scrollSamples, rig.view.samplesPerPixel);
    });
    CHECK(heightWithoutVideo == 0.0f);

    const std::string vt = rig.edit.addVideoTrack("Video 1");
    rig.edit.addVideoClip(vt, videoClip());

    float heightWithVideo = -1.0f;
    rig.frame(-100.0f, -100.0f, false, [&] {
        heightWithVideo = gui::drawVideoLane(
            rig.edit, rig.transport, rig.view,
            ImGui::GetCursorScreenPos(), 1200.0f, kGutterWidth,
            rig.view.scrollSamples, rig.view.samplesPerPixel);
    });
    CHECK(heightWithVideo > 0.0f);
}

TEST_CASE("dragging empty track area makes a time selection",
          "[timelinelayout]") {
    LayoutRig rig;
    rig.view.snapEnabled = true;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = x0 + 300.0f;

    rig.tick(x0, laneY, false);
    rig.tick(x0, laneY, true);
    rig.tick(x0 + 150.0f, laneY, true);
    rig.tick(x1, laneY, true);
    rig.tick(x1, laneY, false);

    CHECK(rig.view.hasSelection);
    // Both edges land on the current format's divisions, so the span is the
    // 300 px drag rounded to them rather than the raw pixel arithmetic.
    const int64_t snap = gui::snapStepFor(rig.view.tcMode,
                                          rig.view.samplesPerPixel);
    const int64_t span =
        std::abs(rig.view.selectionEnd - rig.view.selectionStart);
    CHECK(rig.view.selectionStart % snap == 0);
    CHECK(rig.view.selectionEnd % snap == 0);
    CHECK(span % snap == 0);
    CHECK(std::abs(span - static_cast<int64_t>(300.0 *
                                               rig.view.samplesPerPixel)) < snap);
    // The drag ends with the press released, or the next click would extend
    // this selection instead of starting a new one.
    CHECK_FALSE(rig.view.isSelecting);
}

TEST_CASE("a click without movement seeks and leaves no selection",
          "[timelinelayout]") {
    // The selection drag and the click-to-seek share one press. A plain click
    // must still land on the playhead rather than leaving a zero-width
    // selection highlighted across the arrangement.
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    rig.clickTimelineAt(rig.origin.x + kGutterWidth + 200.0f, laneY);

    CHECK_FALSE(rig.view.hasSelection);
    // The seek is requested on the UI thread; commit it the way the engine
    // would before reading the position back.
    engine::TimeInfo time{};
    rig.transport.advanceAndFill(time, 0, 48000.0);
    CHECK(rig.transport.position() ==
          static_cast<int64_t>(200.0 * rig.view.samplesPerPixel));
}

TEST_CASE("snap off keeps a dragged range at its raw sample positions",
          "[timelinelayout][timelinegrid]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    const float x0 = rig.origin.x + kGutterWidth + 97.0f;
    const float x1 = rig.origin.x + kGutterWidth + 350.0f;
    rig.tick(x0, laneY, false);
    rig.tick(x0, laneY, true);
    rig.tick(x1, laneY, true);
    rig.tick(x1, laneY, false);

    REQUIRE(rig.view.hasSelection);
    CHECK(rig.view.selectionStart == 97 * 500);
    CHECK(rig.view.selectionEnd == 350 * 500);
}

TEST_CASE("snap on seeks to the active timecode frame",
          "[timelinelayout][timelinegrid]") {
    LayoutRig rig;
    rig.view.snapEnabled = true;
    rig.view.tcMode = gui::TimecodeMode::Smpte;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    rig.clickTimelineAt(rig.origin.x + kGutterWidth + 101.0f, laneY);
    engine::TimeInfo time{};
    rig.transport.advanceAndFill(time, 0, 48000.0);

    // 101 px at 500 samples/px is 50,500 samples. At 24 fps the adjacent
    // frame boundaries are 50,000 and 52,000, so the nearest is 50,000.
    CHECK(rig.transport.position() == 50000);
}

TEST_CASE("option-clicking a marker deletes it", "[timelinelayout]") {
    LayoutRig rig;
    const std::string mt = rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    document::Marker m;
    m.name = "Cue";
    m.position = 100000;
    rig.edit.addMarker(mt, m);
    rig.settle();

    const float markerX =
        rig.origin.x + kGutterWidth +
        static_cast<float>(100000.0 / rig.view.samplesPerPixel);
    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight * 0.5f;

    // Bare click on the same pixel selects and drags — it must not delete.
    rig.clickTimelineAt(markerX, laneY);
    REQUIRE(rig.edit.markerTrack(mt)->markers.size() == 1);

    rig.holdAlt(true);
    rig.clickTimelineAt(markerX, laneY);
    rig.holdAlt(false);

    CHECK(rig.edit.markerTrack(mt)->markers.empty());

    // And it comes back where it was, not at the pointer.
    rig.undo.undo();
    REQUIRE(rig.edit.markerTrack(mt)->markers.size() == 1);
    CHECK(rig.edit.markerTrack(mt)->markers.front().position == 100000);
}

TEST_CASE("a lane drag selects only that lane; the ruler selects all",
          "[timelinelayout]") {
    LayoutRig rig;
    rig.view.snapEnabled = true;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.edit.addTrack("Audio 2");
    rig.settle();

    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = x0 + 300.0f;

    auto dragAcross = [&](float y) {
        rig.tick(x0, y, false);
        rig.tick(x0, y, true);
        rig.tick(x0 + 150.0f, y, true);
        rig.tick(x1, y, true);
        rig.tick(x1, y, false);
    };

    SECTION("dragging the first lane scopes the selection to row 0") {
        dragAcross(tracksTop + kTrackHeight * 0.5f);
        CHECK(rig.view.hasSelection);
        CHECK(rig.view.selectionRow == 0);
        // Dragging a lane also makes it the selected track.
        CHECK(rig.view.selectedTrackIndex == 0);
    }

    SECTION("dragging the second lane scopes the selection to row 1") {
        dragAcross(tracksTop + kTrackHeight * 1.5f);
        CHECK(rig.view.hasSelection);
        CHECK(rig.view.selectionRow == 1);
        CHECK(rig.view.selectedTrackIndex == 1);
    }

    SECTION("a vertical wander does not move the selection to another lane") {
        const float y = tracksTop + kTrackHeight * 0.5f;
        rig.tick(x0, y, false);
        rig.tick(x0, y, true);
        // Drag down through the second lane and back.
        rig.tick(x0 + 150.0f, y + kTrackHeight, true);
        rig.tick(x1, y + kTrackHeight, true);
        rig.tick(x1, y + kTrackHeight, false);
        CHECK(rig.view.hasSelection);
        CHECK(rig.view.selectionRow == 0);
    }

    SECTION("dragging the ruler selects across every track") {
        dragAcross(rig.origin.y + kRulerHeight * 0.5f);
        CHECK(rig.view.hasSelection);
        CHECK(rig.view.selectionRow == -1);
        const int64_t snap = gui::snapStepFor(rig.view.tcMode,
                                              rig.view.samplesPerPixel);
        const int64_t span =
            std::abs(rig.view.selectionEnd - rig.view.selectionStart);
        CHECK(span % snap == 0);
        CHECK(std::abs(span - static_cast<int64_t>(
                  300.0 * rig.view.samplesPerPixel)) < snap);
    }

    SECTION("dragging below the last track selects nothing") {
        // Audio rows are followed by the permanent Main bus row.
        dragAcross(tracksTop + kTrackHeight * 3.0f + 20.0f);
        CHECK_FALSE(rig.view.hasSelection);
    }
}

// ─── Grid ───────────────────────────────────────────────────────────────────
// gridStepFor is the whole point of the timing-format grids: each format has
// to land on divisions that are whole in its own units. A seconds grid drawn
// under a bars|beats ruler puts every label at a position that is not a beat.

TEST_CASE("each timing format grids in its own units", "[timelinegrid]") {
    constexpr double kSr = 48000.0;
    constexpr double kFps = 24.0;
    constexpr double kBpm = 120.0;
    constexpr double kSpp = 500.0;
    const int64_t frame = static_cast<int64_t>(kSr / kFps);   // 2000
    const int64_t beat = static_cast<int64_t>(kSr * 60.0 / kBpm); // 24000

    SECTION("min:sec grids on whole seconds") {
        const auto g = gui::gridStepFor(gui::TimecodeMode::MinSec, kSpp, kSr,
                                        kFps, kBpm);
        CHECK(g.major == static_cast<int64_t>(2 * kSr));
        CHECK(g.major % g.minor == 0);
    }

    SECTION("SMPTE grids on whole frames, never between them") {
        const auto g = gui::gridStepFor(gui::TimecodeMode::Smpte, kSpp, kSr,
                                        kFps, kBpm);
        CHECK(g.major % frame == 0);
        CHECK(g.minor % frame == 0);
    }

    SECTION("bars|beats grids on whole beats") {
        const auto g = gui::gridStepFor(gui::TimecodeMode::BarsBeats, kSpp, kSr,
                                        kFps, kBpm);
        CHECK(g.major % beat == 0);
        CHECK(g.major == 4 * beat);   // one bar of 4/4
        CHECK(g.minor == beat);       // subdivided into beats
    }

    SECTION("feet+frames grids on whole frames") {
        const auto g = gui::gridStepFor(gui::TimecodeMode::FeetFrames, kSpp,
                                        kSr, kFps, kBpm);
        CHECK(g.major % frame == 0);
        CHECK(g.minor % frame == 0);
        // 16 frames to the foot: past a foot the step is a whole number of feet.
        CHECK(g.major % (16 * frame) == 0);
    }

    SECTION("samples grids on round decades") {
        const auto g = gui::gridStepFor(gui::TimecodeMode::Samples, kSpp, kSr,
                                        kFps, kBpm);
        CHECK(g.major == 100000);
        CHECK(g.minor == 20000);
    }
}

TEST_CASE("a fractional frame rate still grids on whole frames",
          "[timelinegrid]") {
    // 29.97 is where a "major = a whole second, minor = major/5" grid breaks:
    // a whole second is 29.97 frames, so the minor stops dividing the major
    // and a single loop testing `sample % major` drops every labelled tick.
    // The two divisions are walked separately for exactly this reason.
    constexpr double kSr = 48000.0;
    constexpr double kFps = 29.97;
    const double frame = kSr / kFps;

    // At this zoom the major comes off the seconds ladder, which is what makes
    // the label read 00:00:02:00 rather than landing mid-second. Whole frames
    // is the wrong demand on it; whole seconds is the right one.
    const auto coarse = gui::gridStepFor(gui::TimecodeMode::Smpte, 500.0, kSr,
                                         kFps);
    CHECK(coarse.major % static_cast<int64_t>(kSr) == 0);

    // The minor is always whole frames, at every zoom — a tick between two
    // frames marks a position SMPTE cannot express.
    for (double spp : {5.0, 50.0, 500.0, 5000.0}) {
        const auto g = gui::gridStepFor(gui::TimecodeMode::Smpte, spp, kSr,
                                        kFps);
        const double minorFrames = g.minor / frame;
        CHECK(std::abs(minorFrames - std::llround(minorFrames)) < 0.01);
        CHECK(g.minor > 0);
    }

    // And zoomed in far enough, the major itself is frame-based.
    const auto fine = gui::gridStepFor(gui::TimecodeMode::Smpte, 5.0, kSr, kFps);
    const double majorFrames = fine.major / frame;
    CHECK(std::abs(majorFrames - std::llround(majorFrames)) < 0.01);
}

TEST_CASE("the grid follows the format, not just the zoom", "[timelinegrid]") {
    // At 90 bpm a bar is not a round number of seconds, so the two formats
    // must disagree — if they match here, the format is being ignored.
    constexpr double kSr = 48000.0;
    const auto seconds =
        gui::gridStepFor(gui::TimecodeMode::MinSec, 500.0, kSr, 24.0, 90.0);
    const auto bars =
        gui::gridStepFor(gui::TimecodeMode::BarsBeats, 500.0, kSr, 24.0, 90.0);
    CHECK(seconds.major != bars.major);
    const int64_t beat = static_cast<int64_t>(kSr * 60.0 / 90.0);
    CHECK(bars.major % beat == 0);
}

TEST_CASE("the grid coarsens as the view zooms out", "[timelinegrid]") {
    constexpr double kSr = 48000.0;
    int64_t previous = 0;
    for (double spp : {50.0, 500.0, 5000.0, 50000.0}) {
        const auto g = gui::gridStepFor(gui::TimecodeMode::MinSec, spp, kSr);
        CHECK(g.major > previous);
        // And a labelled division never crowds below its target spacing.
        CHECK(static_cast<double>(g.major) / spp >= 100.0);
        previous = g.major;
    }
}

TEST_CASE("a selection dragged in timecode lands on whole frames",
          "[timelinegrid]") {
    // The point of the format-aware snap: in timecode a selection boundary is
    // a frame boundary. A range that starts 0.3 of a frame in is a range the
    // format cannot name, and every downstream edit inherits the error.
    constexpr double kSr = 48000.0;
    constexpr double kFps = 24.0;
    const int64_t frame = static_cast<int64_t>(kSr / kFps);

    LayoutRig rig;
    rig.view.snapEnabled = true;
    rig.view.tcMode = gui::TimecodeMode::Smpte;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.settle();

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    // Deliberately awkward pixel offsets, so an unsnapped drag would land
    // between frames.
    const float x0 = rig.origin.x + kGutterWidth + 97.0f;
    const float x1 = x0 + 253.0f;
    rig.tick(x0, laneY, false);
    rig.tick(x0, laneY, true);
    rig.tick((x0 + x1) * 0.5f, laneY, true);
    rig.tick(x1, laneY, true);
    rig.tick(x1, laneY, false);

    REQUIRE(rig.view.hasSelection);
    CHECK(rig.view.selectionStart % frame == 0);
    CHECK(rig.view.selectionEnd % frame == 0);
}

TEST_CASE("the snap increment stays in the format's units at every zoom",
          "[timelinegrid]") {
    constexpr double kSr = 48000.0;
    constexpr double kFps = 24.0;
    constexpr double kBpm = 120.0;
    const int64_t frame = static_cast<int64_t>(kSr / kFps);
    const int64_t beat = static_cast<int64_t>(kSr * 60.0 / kBpm);

    for (double spp : {5.0, 50.0, 500.0, 5000.0}) {
        CHECK(gui::snapStepFor(gui::TimecodeMode::Smpte, spp, kSr, kFps,
                               kBpm) % frame == 0);
        CHECK(gui::snapStepFor(gui::TimecodeMode::FeetFrames, spp, kSr, kFps,
                               kBpm) % frame == 0);
        // Bars|beats subdivides the beat rather than multiplying frames, so
        // the test is that a beat is a whole number of snap steps.
        const int64_t barsSnap =
            gui::snapStepFor(gui::TimecodeMode::BarsBeats, spp, kSr, kFps, kBpm);
        CHECK((beat % barsSnap == 0 || barsSnap % beat == 0));
    }

    // It is never finer than the pointer can aim at, and never coarser than
    // the visible labelled division.
    for (double spp : {5.0, 50.0, 500.0, 5000.0}) {
        const int64_t snap =
            gui::snapStepFor(gui::TimecodeMode::MinSec, spp, kSr);
        const auto grid = gui::gridStepFor(gui::TimecodeMode::MinSec, spp, kSr);
        CHECK(static_cast<double>(snap) / spp >= 3.0);
        CHECK(snap <= grid.major);
    }
}

TEST_CASE("format snapping rounds positions in the active ruler units",
          "[timelinegrid]") {
    constexpr double kSr = 48000.0;
    constexpr double kSpp = 500.0;

    // SMPTE and feet+frames share exact whole-frame boundaries at 24 fps.
    CHECK(gui::snapSampleToFormat(50'500, gui::TimecodeMode::Smpte,
                                  kSpp, kSr, 24.0) == 50'000);
    CHECK(gui::snapSampleToFormat(51'100, gui::TimecodeMode::FeetFrames,
                                  kSpp, kSr, 24.0) == 52'000);

    // At 120 bpm, the finest aimable musical division here is a sixteenth
    // note (6,000 samples), not an arbitrary sample interval.
    CHECK(gui::snapSampleToFormat(5'100, gui::TimecodeMode::BarsBeats,
                                  kSpp, kSr, 24.0, 120.0) == 6'000);

    // The remaining formats stay on their own round decimal ladders.
    CHECK(gui::snapSampleToFormat(3'000, gui::TimecodeMode::MinSec,
                                  kSpp, kSr) == 2'400);
    CHECK(gui::snapSampleToFormat(3'100, gui::TimecodeMode::Samples,
                                  kSpp, kSr) == 4'000);
    CHECK(gui::snapSampleToFormat(-100, gui::TimecodeMode::Samples,
                                  kSpp, kSr) == 0);
}

TEST_CASE("the zoom range reaches a feature-length session", "[timelinegrid]") {
    // The old 50,000 ceiling showed ~17 minutes across a 1000 px timeline.
    // A 2-hour session has to fit end to end.
    constexpr double kSr = 48000.0;
    const double widestSpan = gui::kMaxSamplesPerPixel * 1000.0;
    CHECK(widestSpan / kSr >= 2.0 * 3600.0);
    CHECK(gui::kMinSamplesPerPixel < gui::kMaxSamplesPerPixel);

    // And the grid still has a division to offer out there, rather than
    // falling off the end of its ladder into raw samples.
    const auto g = gui::gridStepFor(gui::TimecodeMode::MinSec,
                                    gui::kMaxSamplesPerPixel, kSr);
    CHECK(g.major > 0);
    CHECK(g.major % static_cast<int64_t>(kSr) == 0);   // whole seconds
}

TEST_CASE("the timeline reads its sample rate from the document",
          "[timelinegrid]") {
    // The ruler, grid and snap all key off the session rate. A document at
    // 96 k must not be measured with a hardcoded 48 k, or every label is
    // half of what it should say.
    LayoutRig rig;
    rig.view.snapEnabled = true;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.view.tcMode = gui::TimecodeMode::Smpte;
    rig.edit.setSampleRate(96000);
    rig.settle();

    const int64_t frame96 = 96000 / 24;          // 4000 samples
    // 100 px in at 500 spp is sample 50000. Snapped to 96 k frames that is
    // 52000 (13 frames); snapped to 48 k frames it would be 50000 (25 frames).
    // Divisibility alone would not tell the two apart — 50000 is a multiple of
    // the 48 k frame and the assertion would pass against a hardcoded rate.
    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    rig.tick(x0, laneY, false);
    rig.tick(x0, laneY, true);
    rig.tick(x0 + 250.0f, laneY, true);
    rig.tick(x0 + 250.0f, laneY, false);

    REQUIRE(rig.view.hasSelection);
    CHECK(rig.view.selectionStart == 13 * frame96);
    CHECK(rig.view.selectionEnd % frame96 == 0);
}

TEST_CASE("zooming keeps a visible playhead at the same screen position",
          "[timelinegrid]") {
    gui::TimelineViewState view;
    view.laneWidthPixels = 1000.0f;
    view.samplesPerPixel = 500.0;
    constexpr int64_t kPlayhead = 4'000'000;
    constexpr double kPlayheadPixel = 320.0;
    view.scrollSamples =
        kPlayhead - kPlayheadPixel * view.samplesPerPixel;

    // Screen offset of a sample from the left edge of the lane.
    auto pixelOf = [&](int64_t sample) {
        return (sample - view.scrollSamples) / view.samplesPerPixel;
    };

    gui::zoomAroundSample(view, 1000.0, kPlayhead);
    CHECK(view.samplesPerPixel == 1000.0);
    CHECK(pixelOf(kPlayhead) == Catch::Approx(kPlayheadPixel));

    // The edit stays visually pinned under the playhead through repeated
    // steps in instead of jumping the cursor to the lane centre.
    for (int i = 0; i < 6; ++i) {
        gui::zoomAroundSample(view, view.samplesPerPixel * 0.5, kPlayhead);
        REQUIRE(pixelOf(kPlayhead) == Catch::Approx(kPlayheadPixel));
    }
    // And back out again. Zoomed out far enough, preserving the exact pixel
    // would require scrolling before sample zero. The scroll pins at zero and
    // the playhead moves left only because the timeline has a hard boundary.
    for (int i = 0; i < 10; ++i) {
        gui::zoomAroundSample(view, view.samplesPerPixel * 2.0, kPlayhead);
        if (view.scrollSamples > 0.0) {
            REQUIRE(pixelOf(kPlayhead) == Catch::Approx(kPlayheadPixel));
        } else {
            REQUIRE(pixelOf(kPlayhead) <= kPlayheadPixel);
            REQUIRE(pixelOf(kPlayhead) >= 0.0);
        }
    }
}

TEST_CASE("zooming centres an off-screen playhead", "[timelinegrid]") {
    gui::TimelineViewState view;
    view.laneWidthPixels = 1000.0f;
    view.samplesPerPixel = 500.0;
    view.scrollSamples = 0.0;
    constexpr int64_t kPlayhead = 4'000'000;

    gui::zoomAroundSample(view, 1000.0, kPlayhead);
    const double playheadPixel =
        (kPlayhead - view.scrollSamples) / view.samplesPerPixel;
    CHECK(playheadPixel == Catch::Approx(500.0));
}

TEST_CASE("zoom respects the range limits and the start of the timeline",
          "[timelinegrid]") {
    gui::TimelineViewState view;
    view.laneWidthPixels = 1000.0f;

    // Near the top of the session the playhead cannot be centred without
    // scrolling before sample zero, which would show timeline that does not
    // exist. The scroll clamps instead.
    gui::zoomAroundSample(view, 500.0, 1000);
    CHECK(view.scrollSamples == 0.0);

    gui::zoomAroundSample(view, 0.001, 4'000'000);
    CHECK(view.samplesPerPixel == gui::kMinSamplesPerPixel);
    gui::zoomAroundSample(view, 1e12, 4'000'000);
    CHECK(view.samplesPerPixel == gui::kMaxSamplesPerPixel);

    // With no width reported yet there is no centre to aim at; the zoom still
    // applies but the scroll is left alone rather than guessed.
    gui::TimelineViewState fresh;
    fresh.scrollSamples = 12345.0;
    gui::zoomAroundSample(fresh, 800.0, 4'000'000);
    CHECK(fresh.samplesPerPixel == 800.0);
    CHECK(fresh.scrollSamples == 12345.0);
}

TEST_CASE("ctrl+wheel zoom preserves the playhead screen position",
          "[timelinegrid]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.seekTo(4'000'000);
    rig.settle();
    REQUIRE(rig.view.laneWidthPixels > 0.0f);   // the widget reported its width
    constexpr double kPlayheadPixel = 200.0;
    rig.view.scrollSamples =
        4'000'000 - kPlayheadPixel * rig.view.samplesPerPixel;

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    const float x = rig.origin.x + kGutterWidth + 200.0f;

    // ImGui trickles its event queue: a wheel and a mouse-move submitted for
    // the same frame are split across two. Park the pointer first, then send
    // the wheel on its own, or it lands on a frame where nothing is hovered.
    rig.tick(x, laneY, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    rig.tick(x, laneY, false);

    const double before = rig.view.samplesPerPixel;
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 3.0f);
    rig.tick(x, laneY, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);

    CHECK(rig.view.samplesPerPixel < before);    // wheeled in
    const double playheadPixel =
        (4'000'000 - rig.view.scrollSamples) / rig.view.samplesPerPixel;
    CHECK(playheadPixel ==
          Catch::Approx(kPlayheadPixel).margin(1.0));
}

TEST_CASE("finishing a selection puts the playhead at its head",
          "[timelinelayout]") {
    // So that pressing play — or turning on loop — starts at the top of the
    // range rather than wherever the transport happened to be.
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio 1");
    rig.seekTo(2'000'000);          // somewhere far from the drag
    rig.settle();

    const float laneY = rig.origin.y + kRulerHeight + kMarkerLaneHeight +
                        kTrackHeight * 0.5f;
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;

    auto dragBetween = [&](float from, float to) {
        rig.tick(from, laneY, false);
        rig.tick(from, laneY, true);
        rig.tick((from + to) * 0.5f, laneY, true);
        rig.tick(to, laneY, true);
        rig.tick(to, laneY, false);
        engine::TimeInfo time{};
        rig.transport.advanceAndFill(time, 0, 48000.0);
    };

    SECTION("dragging left to right") {
        dragBetween(x0, x0 + 300.0f);
        REQUIRE(rig.view.hasSelection);
        CHECK(rig.transport.position() ==
              std::min(rig.view.selectionStart, rig.view.selectionEnd));
        CHECK(rig.transport.position() < rig.view.selectionEnd);
    }

    SECTION("dragging right to left lands on the same edge") {
        // The head is the lower edge, not the point the drag ended on.
        dragBetween(x0 + 300.0f, x0);
        REQUIRE(rig.view.hasSelection);
        const int64_t head =
            std::min(rig.view.selectionStart, rig.view.selectionEnd);
        CHECK(rig.transport.position() == head);
        CHECK(head < std::max(rig.view.selectionStart, rig.view.selectionEnd));
    }

    SECTION("a click that is not a drag still seeks to the click") {
        rig.clickTimelineAt(x0, laneY);
        engine::TimeInfo time{};
        rig.transport.advanceAndFill(time, 0, 48000.0);
        CHECK_FALSE(rig.view.hasSelection);
        CHECK(rig.transport.position() ==
              static_cast<int64_t>(100.0 * rig.view.samplesPerPixel));
    }

    SECTION("a drag below the last track leaves the playhead alone") {
        // No lane means no selection, so there is no head to go to. The
        // press still counts as a click and seeks there.
        // The permanent Main bus follows the audio row.
        const float belowTracks = rig.origin.y + kRulerHeight +
                                  kMarkerLaneHeight + kTrackHeight * 2.0f + 20.0f;
        rig.tick(x0, belowTracks, false);
        rig.tick(x0, belowTracks, true);
        rig.tick(x0 + 300.0f, belowTracks, true);
        rig.tick(x0 + 300.0f, belowTracks, false);
        CHECK_FALSE(rig.view.hasSelection);
    }
}

TEST_CASE("the track colour band toggles the disclosure state",
          "[timelinelayout]") {
    // The band is the click target for the arrow. This guards the affordance
    // independently from the lane interaction tests below.
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t0 = rig.edit.addTrack("Audio 1");
    const std::string t1 = rig.edit.addTrack("Audio 2");
    rig.settle();

    const float tracksTop = rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    // The band sits 8 px in from the gutter's left edge and is 18 px wide.
    const float bandX = rig.origin.x + 17.0f;

    CHECK(rig.view.expandedTracks.empty());

    rig.clickTimelineAt(bandX, tracksTop + kTrackHeight * 0.5f);
    CHECK(rig.view.expandedTracks.count(t0) == 1);
    CHECK(rig.view.expandedTracks.count(t1) == 0);   // its neighbour is untouched

    // Clicking again closes it.
    rig.clickTimelineAt(bandX, tracksTop + kTrackHeight * 0.5f);
    CHECK(rig.view.expandedTracks.count(t0) == 0);

    // And the second track has its own band.
    rig.clickTimelineAt(bandX, tracksTop + kTrackHeight * 1.5f);
    CHECK(rig.view.expandedTracks.count(t1) == 1);
    CHECK(rig.view.expandedTracks.count(t0) == 0);
}

TEST_CASE("track disclosures open editable volume automation lanes",
          "[timelinelayout][automation]") {
    constexpr float bandInsetX = 17.0f;
    constexpr float laneClickOffsetX = 180.0f;

    SECTION("audio track") {
        LayoutRig rig;
        rig.edit.addMarkerTrack("Markers");
        const std::string audio = rig.edit.addTrack("Dialog");
        rig.seekTo(12345);
        rig.settle();
        const float tracksTop =
            rig.origin.y + kRulerHeight + kMarkerLaneHeight;
        rig.clickTimelineAt(rig.origin.x + bandInsetX,
                            tracksTop + kTrackHeight * 0.5f);
        REQUIRE(rig.view.expandedTracks.contains(audio));

        rig.clickTimelineAt(
            rig.origin.x + kGutterWidth + laneClickOffsetX,
            tracksTop + kTrackHeight + kAutomationLaneHeight * 0.5f);
        REQUIRE(rig.edit.track(audio)->volumeAutomation.size() == 1);

        // Editing an envelope must not leak through to the generic timeline
        // click handler and move the transport at the same time.
        engine::TimeInfo time{};
        rig.transport.advanceAndFill(time, 0, 48000.0);
        CHECK(rig.transport.position() == 12345);
        rig.undo.undo();
        CHECK(rig.edit.track(audio)->volumeAutomation.empty());
    }

    SECTION("MIDI instrument track") {
        LayoutRig rig;
        rig.edit.addMarkerTrack("Markers");
        const std::string midi = rig.edit.addMidiTrack("Instrument");
        rig.settle();
        const float tracksTop =
            rig.origin.y + kRulerHeight + kMarkerLaneHeight;
        const float midiHeight =
            kTrackHeight + ImGui::GetFontSize() + 2.0f + 3.0f;
        rig.clickTimelineAt(rig.origin.x + bandInsetX,
                            tracksTop + midiHeight * 0.5f);
        REQUIRE(rig.view.expandedTracks.contains(midi));
        rig.clickTimelineAt(
            rig.origin.x + kGutterWidth + laneClickOffsetX,
            tracksTop + midiHeight + kAutomationLaneHeight * 0.5f);
        REQUIRE(rig.edit.midiTrack(midi)->volumeAutomation.size() == 1);
    }

    SECTION("user bus and Main") {
        LayoutRig rig;
        rig.edit.addMarkerTrack("Markers");
        const std::string bus = rig.edit.addBus("DX Stem");
        rig.settle();
        const float tracksTop =
            rig.origin.y + kRulerHeight + kMarkerLaneHeight;

        rig.clickTimelineAt(rig.origin.x + bandInsetX,
                            tracksTop + kTrackHeight * 0.5f);
        REQUIRE(rig.view.expandedTracks.contains(bus));
        rig.clickTimelineAt(
            rig.origin.x + kGutterWidth + laneClickOffsetX,
            tracksTop + kTrackHeight + kAutomationLaneHeight * 0.5f);
        REQUIRE(rig.edit.bus(bus)->volumeAutomation.size() == 1);

        const float mainTop =
            tracksTop + kTrackHeight + kAutomationLaneHeight;
        rig.clickTimelineAt(rig.origin.x + bandInsetX,
                            mainTop + kTrackHeight * 0.5f);
        REQUIRE(rig.view.expandedTracks.contains(document::kMainBusId));
        rig.clickTimelineAt(
            rig.origin.x + kGutterWidth + laneClickOffsetX,
            mainTop + kTrackHeight + kAutomationLaneHeight * 0.5f);
        REQUIRE(rig.edit.mainBus()->volumeAutomation.size() == 1);
    }
}

TEST_CASE("automation lane toggles between pencil line and curve tools",
          "[timelinelayout][automation][tools]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    REQUIRE(rig.view.expandedTracks.contains(track));
    REQUIRE(rig.view.automationTool == gui::AutomationTool::Pencil);

    // Parameter selector occupies x=28..140. The three 24 px tool buttons sit
    // immediately to its right: Pencil, Line, then Curve.
    rig.clickTimelineAt(rig.origin.x + 192.0f, automationTop + 16.0f);
    CHECK(rig.view.automationTool == gui::AutomationTool::Line);
    rig.clickTimelineAt(rig.origin.x + 216.0f, automationTop + 16.0f);
    CHECK(rig.view.automationTool == gui::AutomationTool::Curve);
    rig.clickTimelineAt(rig.origin.x + 160.0f, automationTop + 16.0f);
    CHECK(rig.view.automationTool == gui::AutomationTool::Pencil);
    CHECK_FALSE(rig.undo.canUndo());
}

TEST_CASE("pencil tool uses its pencil as the automation lane cursor",
          "[timelinelayout][automation][tools][cursor]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    REQUIRE(rig.view.expandedTracks.contains(track));

    const float laneX = rig.origin.x + kGutterWidth + 80.0f;
    const float laneY = automationTop + kAutomationLaneHeight * 0.5f;
    rig.tick(laneX, laneY, false);
    CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_None);

    rig.view.automationTool = gui::AutomationTool::Line;
    rig.tick(laneX, laneY, false);
    CHECK(ImGui::GetMouseCursor() != ImGuiMouseCursor_None);

    rig.view.automationTool = gui::AutomationTool::Pencil;
    rig.tick(rig.origin.x + 40.0f, tracksTop + 10.0f, false);
    CHECK(ImGui::GetMouseCursor() != ImGuiMouseCursor_None);
}

TEST_CASE("pencil draws a thinned envelope as one undoable gesture",
          "[timelinelayout][automation][tools]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.edit.addVolumeAutomationPoint(track, 0, -12.0);
    rig.edit.addVolumeAutomationPoint(track, 200000, -6.0);
    const auto before = rig.edit.track(track)->volumeAutomation;
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);

    auto volumeY = [&](double db) {
        return automationTop + 8.0f +
            static_cast<float>((document::kMaxVolumeAutomationDb - db) /
                (document::kMaxVolumeAutomationDb -
                 document::kMinVolumeAutomationDb)) *
                (kAutomationLaneHeight - 16.0f);
    };
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = rig.origin.x + kGutterWidth + 200.0f;
    const float x2 = rig.origin.x + kGutterWidth + 300.0f;
    rig.tick(x0, volumeY(-36.0), false);
    rig.tick(x0, volumeY(-36.0), true);
    rig.tick(x1, volumeY(0.0), true);
    rig.tick(x2, volumeY(-18.0), true);
    rig.tick(x2, volumeY(-18.0), false);

    const auto& drawn = rig.edit.track(track)->volumeAutomation;
    REQUIRE(drawn.size() > 6);
    CHECK(rig.undo.undoDepth() == 1);
    CHECK(drawn.front() == before.front());
    CHECK(drawn.back() == before.back());
    CHECK(std::adjacent_find(
              drawn.begin(), drawn.end(), [](const auto& a, const auto& b) {
                  return a.sample >= b.sample;
              }) == drawn.end());
    const auto start = std::find_if(drawn.begin(), drawn.end(),
                                    [](const auto& point) {
                                        return point.sample == 50000;
                                    });
    const auto end = std::find_if(drawn.begin(), drawn.end(),
                                  [](const auto& point) {
                                      return point.sample == 150000;
                                  });
    REQUIRE(start != drawn.end());
    REQUIRE(end != drawn.end());
    // ImGui rounds row origins to display pixels, so permit one pixel of
    // vertical quantization (about 1.2 dB at this lane height).
    CHECK(start->db == Catch::Approx(-36.0).margin(1.2));
    CHECK(end->db == Catch::Approx(-18.0).margin(1.2));

    rig.undo.undo();
    CHECK(rig.edit.track(track)->volumeAutomation == before);
}

TEST_CASE("line tool replaces its range with one straight ramp",
          "[timelinelayout][automation][tools]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.edit.addVolumeAutomationPoint(track, 0, -12.0);
    rig.edit.addVolumeAutomationPoint(track, 100000, -48.0);
    rig.edit.addVolumeAutomationPoint(track, 200000, -6.0);
    const auto before = rig.edit.track(track)->volumeAutomation;
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    rig.clickTimelineAt(rig.origin.x + 192.0f, automationTop + 16.0f);
    REQUIRE(rig.view.automationTool == gui::AutomationTool::Line);

    auto volumeY = [&](double db) {
        return automationTop + 8.0f +
            static_cast<float>((document::kMaxVolumeAutomationDb - db) /
                (document::kMaxVolumeAutomationDb -
                 document::kMinVolumeAutomationDb)) *
                (kAutomationLaneHeight - 16.0f);
    };
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = rig.origin.x + kGutterWidth + 300.0f;
    rig.tick(x0, volumeY(-24.0), false);
    rig.tick(x0, volumeY(-24.0), true);
    rig.tick(x1, volumeY(0.0), true);
    rig.tick(x1, volumeY(0.0), false);

    const auto& ramp = rig.edit.track(track)->volumeAutomation;
    REQUIRE(ramp.size() == 4);
    CHECK(rig.undo.undoDepth() == 1);
    CHECK(ramp[0] == before[0]);
    CHECK(ramp[1].sample == 50000);
    CHECK(ramp[1].db == Catch::Approx(-24.0).margin(1.2));
    CHECK(ramp[2].sample == 150000);
    CHECK(ramp[2].db == Catch::Approx(0.0).margin(1.2));
    CHECK(ramp[3] == before[2]);
    const std::string firstRampId = ramp[1].id;
    const std::string lastRampId = ramp[2].id;
    CHECK_FALSE(firstRampId.empty());
    CHECK_FALSE(lastRampId.empty());

    rig.undo.undo();
    CHECK(rig.edit.track(track)->volumeAutomation == before);
    rig.undo.redo();
    CHECK(rig.edit.track(track)->volumeAutomation[1].id == firstRampId);
    CHECK(rig.edit.track(track)->volumeAutomation[2].id == lastRampId);
}

TEST_CASE("line tool draws pan automation with normalized values",
          "[timelinelayout][automation][tools][pan]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.view.automationParameters[track] = gui::AutomationParameter::Pan;
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    rig.clickTimelineAt(rig.origin.x + 192.0f, automationTop + 16.0f);
    REQUIRE(rig.view.automationTool == gui::AutomationTool::Line);

    auto panY = [&](double pan) {
        return automationTop + 8.0f +
            static_cast<float>((1.0 - pan) / 2.0) *
                (kAutomationLaneHeight - 16.0f);
    };
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = rig.origin.x + kGutterWidth + 300.0f;
    rig.tick(x0, panY(-0.75), false);
    rig.tick(x0, panY(-0.75), true);
    rig.tick(x1, panY(0.5), true);
    rig.tick(x1, panY(0.5), false);

    const auto& ramp = rig.edit.track(track)->panAutomation;
    REQUIRE(ramp.size() == 2);
    CHECK(ramp[0].sample == 50000);
    CHECK(ramp[0].pan == Catch::Approx(-0.75).margin(0.04));
    CHECK(ramp[1].sample == 150000);
    CHECK(ramp[1].pan == Catch::Approx(0.5).margin(0.04));
    CHECK(rig.undo.undoDepth() == 1);
    rig.undo.undo();
    CHECK(rig.edit.track(track)->panAutomation.empty());
}

TEST_CASE("curve tool draws a parabolic ramp as one undoable gesture",
          "[timelinelayout][automation][tools][curve]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    rig.clickTimelineAt(rig.origin.x + 216.0f, automationTop + 16.0f);
    REQUIRE(rig.view.automationTool == gui::AutomationTool::Curve);

    auto volumeY = [&](double db) {
        return automationTop + 8.0f +
            static_cast<float>((document::kMaxVolumeAutomationDb - db) /
                (document::kMaxVolumeAutomationDb -
                 document::kMinVolumeAutomationDb)) *
                (kAutomationLaneHeight - 16.0f);
    };
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = rig.origin.x + kGutterWidth + 300.0f;
    rig.tick(x0, volumeY(-48.0), false);
    rig.tick(x0, volumeY(-48.0), true);
    rig.tick(x1, volumeY(0.0), true);
    rig.tick(x1, volumeY(0.0), false);

    const auto& curve = rig.edit.track(track)->volumeAutomation;
    REQUIRE(curve.size() > 10);
    CHECK(rig.undo.undoDepth() == 1);
    CHECK(std::adjacent_find(
              curve.begin(), curve.end(), [](const auto& a, const auto& b) {
                  return a.sample >= b.sample;
              }) == curve.end());
    const auto midpoint = std::find_if(
        curve.begin(), curve.end(), [](const auto& point) {
            return point.sample == 100000;
        });
    REQUIRE(midpoint != curve.end());
    // t^2 at the halfway point is 0.25: -48 + (48 * 0.25) = -36 dB.
    CHECK(midpoint->db == Catch::Approx(-36.0).margin(1.5));
    CHECK(curve.front().db == Catch::Approx(-48.0).margin(1.5));
    CHECK(curve.back().db == Catch::Approx(0.0).margin(1.5));

    rig.undo.undo();
    CHECK(rig.edit.track(track)->volumeAutomation.empty());
}

TEST_CASE("Command changes the curve tool to logarithmic pan automation",
          "[timelinelayout][automation][tools][curve][pan]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    rig.view.automationParameters[track] = gui::AutomationParameter::Pan;
    rig.settle();
    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    const float automationTop = tracksTop + effectiveTrackHeight();
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    rig.clickTimelineAt(rig.origin.x + 216.0f, automationTop + 16.0f);
    REQUIRE(rig.view.automationTool == gui::AutomationTool::Curve);

    auto panY = [&](double pan) {
        return automationTop + 8.0f +
            static_cast<float>((1.0 - pan) / 2.0) *
                (kAutomationLaneHeight - 16.0f);
    };
    const float x0 = rig.origin.x + kGutterWidth + 100.0f;
    const float x1 = rig.origin.x + kGutterWidth + 300.0f;
    rig.holdSuper(true);
    rig.tick(x0, panY(-1.0), false);
    rig.tick(x0, panY(-1.0), true);
    CHECK(rig.view.automationDrawLogarithmic);
    rig.tick(x1, panY(1.0), true);
    rig.tick(x1, panY(1.0), false);
    rig.holdSuper(false);
    rig.settle();

    const auto& curve = rig.edit.track(track)->panAutomation;
    REQUIRE(curve.size() > 10);
    CHECK(rig.undo.undoDepth() == 1);
    const auto midpoint = std::find_if(
        curve.begin(), curve.end(), [](const auto& point) {
            return point.sample == 100000;
        });
    REQUIRE(midpoint != curve.end());
    const double expected = -1.0 + 2.0 * std::log10(5.5);
    CHECK(midpoint->pan == Catch::Approx(expected).margin(0.05));
    // The same endpoints with the default parabola would be at -0.5 here.
    CHECK(midpoint->pan > 0.4);
    CHECK_FALSE(rig.view.automationDrawLogarithmic);

    rig.undo.undo();
    CHECK(rig.edit.track(track)->panAutomation.empty());
}

TEST_CASE("double-clicking an automation point opens an undoable dB editor",
          "[timelinelayout][automation]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    REQUIRE_FALSE(rig.edit.addVolumeAutomationPoint(track, 90000, 0.0).empty());
    rig.settle();

    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    REQUIRE(rig.view.expandedTracks.contains(track));

    // 90,000 samples at 500 samples/pixel is 180 px into the lane. The
    // envelope spans +6 to -60 dB between its 8 px vertical insets.
    const float pointX = rig.origin.x + kGutterWidth + 180.0f;
    const float automationTop = tracksTop + effectiveTrackHeight();
    const float graphHeight = kAutomationLaneHeight - 16.0f;
    const float pointY = automationTop + 8.0f +
        static_cast<float>((document::kMaxVolumeAutomationDb - 0.0) /
                           (document::kMaxVolumeAutomationDb -
                            document::kMinVolumeAutomationDb)) * graphHeight;
    rig.doubleClickTimelineAt(pointX, pointY);
    REQUIRE(rig.view.editingAutomationValue);

    rig.typeText("-7.5");
    rig.pressKey(ImGuiKey_Enter);
    REQUIRE_FALSE(rig.view.editingAutomationValue);
    REQUIRE(rig.edit.track(track)->volumeAutomation.size() == 1);
    CHECK(rig.edit.track(track)->volumeAutomation.front().db ==
          Catch::Approx(-7.5));

    REQUIRE(rig.undo.canUndo());
    rig.undo.undo();
    CHECK(rig.edit.track(track)->volumeAutomation.front().db ==
          Catch::Approx(0.0));

    // Clicking away is also a commit, but that click must not leak through
    // and create another point in the envelope beneath the editor.
    rig.doubleClickTimelineAt(pointX, pointY);
    REQUIRE(rig.view.editingAutomationValue);
    rig.typeText("-12.0");
    rig.clickTimelineAt(pointX + 120.0f,
                        automationTop + kAutomationLaneHeight * 0.5f);
    CHECK_FALSE(rig.view.editingAutomationValue);
    REQUIRE(rig.edit.track(track)->volumeAutomation.size() == 1);
    CHECK(rig.edit.track(track)->volumeAutomation.front().db ==
          Catch::Approx(-12.0));
}

TEST_CASE("pan automation lane edits normalized pan with percent entry",
          "[timelinelayout][automation][pan]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Dialog");
    REQUIRE_FALSE(
        rig.edit.addPanAutomationPoint(track, 90000, -0.5).empty());
    rig.view.automationParameters[track] = gui::AutomationParameter::Pan;
    rig.settle();

    const float tracksTop =
        rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    rig.clickTimelineAt(rig.origin.x + 17.0f,
                        tracksTop + kTrackHeight * 0.5f);
    REQUIRE(rig.view.expandedTracks.contains(track));

    // 90,000 samples at 500 samples/pixel is 180 px into the lane. Pan -0.5
    // sits three quarters of the way down the -1..+1 graph.
    const float pointX = rig.origin.x + kGutterWidth + 180.0f;
    const float automationTop = tracksTop + effectiveTrackHeight();
    const float graphHeight = kAutomationLaneHeight - 16.0f;
    const float pointY = automationTop + 8.0f + graphHeight * 0.75f;
    rig.doubleClickTimelineAt(pointX, pointY);
    REQUIRE(rig.view.editingAutomationValue);
    CHECK(rig.view.activeAutomationParameter ==
          gui::AutomationParameter::Pan);

    rig.typeText("75");
    rig.pressKey(ImGuiKey_Enter);
    REQUIRE_FALSE(rig.view.editingAutomationValue);
    REQUIRE(rig.edit.track(track)->panAutomation.size() == 1);
    CHECK(rig.edit.track(track)->panAutomation.front().pan ==
          Catch::Approx(0.75));
    CHECK(rig.edit.track(track)->volumeAutomation.empty());

    REQUIRE(rig.undo.canUndo());
    rig.undo.undo();
    CHECK(rig.edit.track(track)->panAutomation.front().pan ==
          Catch::Approx(-0.5));
}

TEST_CASE("clicking the gutter beside the band does not toggle it",
          "[timelinelayout]") {
    // The band shares the header row with the name, the sliders and M/S. A
    // click on any of those must not open the track.
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t0 = rig.edit.addTrack("Audio 1");
    rig.settle();

    const float tracksTop = rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    // Well right of the band, over the name.
    rig.clickTimelineAt(rig.origin.x + 90.0f, tracksTop + 10.0f);
    CHECK(rig.view.expandedTracks.empty());
}

TEST_CASE("the timeline record button arms only its audio row",
          "[timelinelayout][record-arm]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string first = rig.edit.addTrack("Boom");
    const std::string second = rig.edit.addTrack("Lav");
    rig.settle();

    const float tracksTop = rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    // The record circle keeps the 21 px hit slot immediately left of M/S.
    rig.clickTimelineAt(rig.origin.x + 189.5f, tracksTop + 14.5f);

    CHECK(rig.edit.track(first)->recordArm);
    CHECK_FALSE(rig.edit.track(second)->recordArm);
}

TEST_CASE("timeline MIDI rows keep M at its pre-recording position",
          "[timelinelayout][record-arm]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string midi = rig.edit.addMidiTrack("Keys");
    rig.settle();

    const float tracksTop = rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    // The audio R position remains ordinary name space on MIDI.
    rig.clickTimelineAt(rig.origin.x + 189.5f, tracksTop + 14.5f);
    CHECK_FALSE(rig.edit.midiTrack(midi)->mute);
    // M and S did not shift when audio gained R.
    rig.clickTimelineAt(rig.origin.x + 213.5f, tracksTop + 14.5f);
    CHECK(rig.edit.midiTrack(midi)->mute);
    CHECK_FALSE(rig.edit.midiTrack(midi)->solo);
}

TEST_CASE("Option-clicking timeline pan and gain restores their defaults",
          "[timelinelayout][gain][pan][reset]") {
    LayoutRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string track = rig.edit.addTrack("Audio 1");
    rig.edit.track(track)->gain = 0.25;
    rig.edit.track(track)->pan = -0.75;
    rig.settle();

    const float tracksTop = rig.origin.y + kRulerHeight + kMarkerLaneHeight;
    // Gutter controls begin at x=96. Gain is the first compact row and pan
    // the second; probe their centers so this tests the real sliders.
    const float controlX = rig.origin.x + 160.0f;
    const float gainY = tracksTop + 31.5f;
    const float panY = tracksTop + 49.5f;

    rig.holdAlt(true);
    rig.clickTimelineAt(controlX, panY);
    rig.clickTimelineAt(controlX, gainY);
    rig.holdAlt(false);

    CHECK(rig.edit.track(track)->pan == 0.0);
    CHECK(rig.edit.track(track)->gain == 1.0);
}
