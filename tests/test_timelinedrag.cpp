// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drag gestures through the REAL drawTimeline, driven headlessly.
//
// The clip-move command has been unit-tested since RB-2, and it was fine —
// what broke was the wiring: three lanes shared one "a drag is happening"
// flag, and the marker lane (which draws first) cancelled every clip drag on
// mouse-up. No command-level test could see that, because no command was ever
// issued. These tests press the mouse down, move it, and release it, then ask
// the document where the clip ended up.
//
// ImGui needs a context but not a graphics backend: give it a DisplaySize and
// a built font atlas and it will run its whole frame into a draw list nobody
// rasterizes.
#include "ImGuiTestRig.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_map>
#include <vector>

using namespace dave;
using dave::testing::ImGuiRig;

namespace {

constexpr float kTrackHeight = 58.0f;
constexpr double kSamplesPerPixel = 500.0;
constexpr float kGutterWidth = 260.0f;

// drawTimeline needs a peak cache and an asset-buffer map it can look nothing
// up in; neither carries state a drag test cares about.
struct TimelineRig : ImGuiRig {
    TimelineRig() { view.samplesPerPixel = kSamplesPerPixel; }

    void tick(float x, float y, bool down) {
        frame(x, y, down, [&] {
            gui::drawTimeline(edit, undo, transport, peaks, view, assetBuffers,
                              kTrackHeight);
        });
    }

    // A complete press-move-release gesture, with enough intermediate frames
    // that ImGui sees a held button rather than a click.
    void dragFrom(ImVec2 from, ImVec2 to) {
        tick(from.x, from.y, false);   // hover, button up
        tick(from.x, from.y, true);    // press
        tick((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f, true);
        tick(to.x, to.y, true);        // held at the destination
        tick(to.x, to.y, false);       // release -> commit
    }

    // Screen X of a timeline sample position. Mirrors drawTimeline's own
    // arithmetic; if they drift, the probes stop landing on clips and the
    // tests fail loudly rather than silently passing.
    float xOfSample(int64_t sample) const {
        return kGutterWidth +
               static_cast<float>((sample - view.scrollSamples) / view.samplesPerPixel);
    }

    // Find the row whose clips respond to a press at `grabX`, by probing down
    // the lane. Hard-coding a lane offset would silently stop matching the
    // layout the first time the ruler or marker lane changes height.
    float findRowY(float grabX, gui::TimelineViewState::DragKind kind) {
        for (float y = 60.0f; y < 400.0f; y += 4.0f) {
            tick(grabX, y, false);
            tick(grabX, y, true);
            const bool started = view.isDragging(kind);
            tick(grabX, y, false);   // release without moving
            if (started) return y;
        }
        return -1.0f;
    }

    gui::PeakCache peaks;
    std::unordered_map<std::string, audio::DecodedAudioAssetPtr> assetBuffers;
};

document::MidiClip midiClip(int64_t start, int64_t length) {
    document::MidiClip c;
    c.name = "Part";
    c.timelineStart = start;
    c.length = length;
    document::MidiNote n;
    n.startSample = 0;
    n.lengthSamples = length / 2;
    n.pitch = 60;
    c.notes.push_back(n);
    return c;
}

} // namespace

TEST_CASE("dragging a MIDI clip moves it and stays moved after release",
          "[timelinedrag]") {
    TimelineRig rig;
    // The marker track is load-bearing, not scenery: the marker lane is what
    // cancelled clip drags, and DaveApp creates a default "Markers" track at
    // startup, so every real session has one. Without it this test cannot
    // reproduce the bug and passes even when the bug is present.
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addMidiTrack("Keys");
    const std::string c = rig.edit.addMidiClip(t, midiClip(0, 480000));

    // Grab the clip near its start and drag 200 px right.
    const float grabX = rig.xOfSample(48000);
    // The MIDI band begins below the ruler + marker lane; probe rows until the
    // press actually starts a drag, rather than hard-coding a lane offset that
    // would silently stop matching the layout.
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::MidiClip);
    REQUIRE(rowCenterY > 0.0f);
    // A press-and-release with no movement must not move the clip.
    REQUIRE(rig.edit.midiClip(t, c)->timelineStart == 0);

    rig.dragFrom(ImVec2(grabX, rowCenterY), ImVec2(grabX + 200.0f, rowCenterY));

    const int64_t expected =
        static_cast<int64_t>(200.0 * kSamplesPerPixel);   // 100000 samples
    const auto* moved = rig.edit.midiClip(t, c);
    REQUIRE(moved != nullptr);
    CHECK(moved->timelineStart == expected);

    // And it is a real undo entry, not a silent write.
    rig.undo.undo();
    CHECK(rig.edit.midiClip(t, c)->timelineStart == 0);
}

TEST_CASE("dragging an audio clip moves it and stays moved after release",
          "[timelinedrag]") {
    // The same defect hit audio clips; it was only noticed on MIDI. Guarding
    // both means a future lane that reaches for the shared drag state breaks a
    // test instead of a user's edit.
    TimelineRig rig;
    // A marker track is not incidental here: the marker lane is what cancelled
    // the drag, and DaveApp creates a default "Markers" track at startup, so
    // every real session has one. Without it this test passes even when the
    // bug is present.
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    const std::string c = rig.edit.addClip(t, clip);

    const float grabX = rig.xOfSample(48000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);
    REQUIRE(rig.edit.clip(t, c)->timelineStart == 0);

    rig.dragFrom(ImVec2(grabX, rowCenterY), ImVec2(grabX + 200.0f, rowCenterY));

    const int64_t expected = static_cast<int64_t>(200.0 * kSamplesPerPixel);
    const auto* moved = rig.edit.clip(t, c);
    REQUIRE(moved != nullptr);
    CHECK(moved->timelineStart == expected);
}

TEST_CASE("clip dragging snaps to the active timecode grid",
          "[timelinedrag][timelinegrid]") {
    TimelineRig rig;
    rig.view.snapEnabled = true;
    rig.view.tcMode = gui::TimecodeMode::Smpte;
    rig.edit.addMarkerTrack("Markers");
    const std::string trackId = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    const std::string clipId = rig.edit.addClip(trackId, clip);

    const float grabX = rig.xOfSample(48000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    rig.dragFrom(ImVec2(grabX, rowCenterY),
                 ImVec2(grabX + 197.0f, rowCenterY));

    const auto* moved = rig.edit.clip(trackId, clipId);
    REQUIRE(moved != nullptr);
    // 197 px is 98,500 samples. A 24 fps frame is 2,000 samples, so this
    // lands on frame 49 rather than preserving the between-frame position.
    CHECK(moved->timelineStart == 98'000);
}

TEST_CASE("a clip drag leaves markers alone", "[timelinedrag]") {
    // The marker lane's mouse-up handler used to run for every drag. It found
    // no marker matching the dragged clip's id, so it moved nothing — and then
    // cleared the shared drag state, which is what killed the clip move. Assert
    // the marker is untouched as well as the clip arriving.
    TimelineRig rig;
    const std::string mt = rig.edit.addMarkerTrack("Markers");
    document::Marker m;
    m.name = "Reel 1";
    m.position = 240000;
    rig.edit.addMarker(mt, m);

    const std::string t = rig.edit.addMidiTrack("Keys");
    const std::string c = rig.edit.addMidiClip(t, midiClip(0, 480000));

    const float grabX = rig.xOfSample(48000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::MidiClip);
    REQUIRE(rowCenterY > 0.0f);

    rig.dragFrom(ImVec2(grabX, rowCenterY), ImVec2(grabX + 200.0f, rowCenterY));

    CHECK(rig.edit.midiClip(t, c)->timelineStart ==
          static_cast<int64_t>(200.0 * kSamplesPerPixel));
    REQUIRE(rig.edit.markerTracks().size() == 1);
    REQUIRE(rig.edit.markerTracks()[0].markers.size() == 1);
    CHECK(rig.edit.markerTracks()[0].markers[0].position == 240000);
}
