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
using dave::testing::ImGuiRig;

namespace {

constexpr float kTrackHeight = 58.0f;
constexpr double kSamplesPerPixel = 500.0;
constexpr float kGutterWidth = 260.0f;
// drawTimeline lays the gutter out from the window's content origin, not from
// the window's left edge, so the timeline's x=0 sits one WindowPadding in.
// The move tests only ever asserted on deltas and so never noticed; the trim
// tests press on a specific pixel and do.
constexpr float kWindowOriginX = 14.0f;

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
        return kWindowOriginX + kGutterWidth +
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

// Trim gestures, driven through the real drawTimeline for the same reason the
// move gestures are: the command is easy to unit-test and was never the part
// that broke. What breaks is the wiring — which edge zone the press landed in,
// whether the drag belongs to this handler, and whether mouse-up commits.
TEST_CASE("dragging a clip's right edge trims its tail", "[timelinedrag]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;   // 960 px at 500 samples/px
    const std::string c = rig.edit.addClip(t, clip);

    const float midX = rig.xOfSample(240000);
    const float rowCenterY =
        rig.findRowY(midX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    // Land inside the 6 px handle at the clip's right edge.
    const float edgeX = rig.xOfSample(480000) - 2.0f;
    rig.dragFrom(ImVec2(edgeX, rowCenterY), ImVec2(edgeX - 100.0f, rowCenterY));

    const auto* trimmed = rig.edit.clip(t, c);
    REQUIRE(trimmed != nullptr);
    // 100 px shorter is 50,000 samples. A tail trim moves nothing else.
    CHECK(trimmed->length == 430'000);
    CHECK(trimmed->timelineStart == 0);
    CHECK(trimmed->sourceOffset == 0);

    rig.undo.undo();
    CHECK(rig.edit.clip(t, c)->length == 480'000);
}

TEST_CASE("dragging a clip's left edge trims its head", "[timelinedrag]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 240000;
    clip.length = 480000;
    clip.sourceOffset = 240000;   // room to trim backwards as well as forwards
    const std::string c = rig.edit.addClip(t, clip);

    const float midX = rig.xOfSample(480000);
    const float rowCenterY =
        rig.findRowY(midX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    const float edgeX = rig.xOfSample(240000) + 2.0f;
    rig.dragFrom(ImVec2(edgeX, rowCenterY), ImVec2(edgeX + 100.0f, rowCenterY));

    const auto* trimmed = rig.edit.clip(t, c);
    REQUIRE(trimmed != nullptr);
    // The box shrinks from the left by 50,000 samples and the audio inside it
    // stays where it was on the timeline, which takes all three values moving
    // together.
    CHECK(trimmed->timelineStart == 290'000);
    CHECK(trimmed->sourceOffset == 290'000);
    CHECK(trimmed->length == 430'000);

    rig.undo.undo();
    const auto* restored = rig.edit.clip(t, c);
    CHECK(restored->timelineStart == 240'000);
    CHECK(restored->sourceOffset == 240'000);
    CHECK(restored->length == 480'000);
}

TEST_CASE("a head trim stops at the start of the source", "[timelinedrag]") {
    // Dragging the head left past the file's own beginning would ask for audio
    // that does not exist. The clip's box has to stop where its source does.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 240000;
    clip.length = 480000;
    clip.sourceOffset = 48000;   // only 48,000 samples of head available
    const std::string c = rig.edit.addClip(t, clip);

    const float midX = rig.xOfSample(480000);
    const float rowCenterY =
        rig.findRowY(midX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    const float edgeX = rig.xOfSample(240000) + 2.0f;
    // 400 px left is 200,000 samples — far more head than the source has.
    rig.dragFrom(ImVec2(edgeX, rowCenterY), ImVec2(edgeX - 400.0f, rowCenterY));

    const auto* trimmed = rig.edit.clip(t, c);
    REQUIRE(trimmed != nullptr);
    CHECK(trimmed->sourceOffset == 0);
    CHECK(trimmed->timelineStart == 192'000);   // 240,000 - 48,000
    CHECK(trimmed->length == 528'000);          // 480,000 + 48,000
}

TEST_CASE("grabbing the middle of a clip still moves it", "[timelinedrag]") {
    // The trim handles must not eat the move gesture. A clip narrow enough
    // that two 6 px handles would meet keeps a grabbable middle third.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 9000;   // 18 px wide
    const std::string c = rig.edit.addClip(t, clip);

    const float midX = rig.xOfSample(4500);
    const float rowCenterY =
        rig.findRowY(midX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    rig.dragFrom(ImVec2(midX, rowCenterY), ImVec2(midX + 100.0f, rowCenterY));

    const auto* moved = rig.edit.clip(t, c);
    REQUIRE(moved != nullptr);
    CHECK(moved->timelineStart == 50'000);
    CHECK(moved->length == 9000);   // moved, not trimmed
}
