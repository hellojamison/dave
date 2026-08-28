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

    // Transport::seek only requests a move; the RT thread commits it. Nothing
    // pumps audio here, so the test does one block itself — otherwise
    // position() stays at 0 and a playhead assertion passes for the wrong
    // reason.
    int64_t committedPosition() {
        engine::TimeInfo time{};
        transport.advanceAndFill(time, 0, 48000.0);
        return transport.position();
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

TEST_CASE("the selection follows a clip as it is trimmed",
          "[timelinedrag][selection]") {
    // Clicking a clip selected its range; trimming an edge should drag that
    // edge of the selection too, not leave it on the clip's old extent.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    rig.edit.addClip(t, clip);

    const float midX = rig.xOfSample(240000);
    const float rowY =
        rig.findRowY(midX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowY > 0.0f);

    // Trim the tail inward by 100 px.
    const float edgeX = rig.xOfSample(480000) - 2.0f;
    rig.dragFrom(ImVec2(edgeX, rowY), ImVec2(edgeX - 100.0f, rowY));

    const auto& trimmed = rig.edit.track(t)->clips.front();
    REQUIRE(trimmed.length < 480000);  // it shrank
    CHECK(rig.view.hasSelection);
    CHECK(rig.view.selectionStart == trimmed.timelineStart);
    CHECK(rig.view.selectionEnd == trimmed.timelineStart + trimmed.length);
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

TEST_CASE("dragging one clip of a group drags the whole group",
          "[timelinedrag][clipgroups]") {
    // The point of a group, and the thing that has to go through the real
    // widget: the drag commits a group move rather than a clip move, and the
    // members that were never touched come along.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    const std::string ca = rig.edit.addClip(a, clip);
    clip.timelineStart = 240000;
    const std::string cb = rig.edit.addClip(b, clip);
    REQUIRE_FALSE(rig.edit.addClipGroup({{a, ca, false}, {b, cb, false}},
                                        0, 720000).empty());

    const float grabX = rig.xOfSample(48000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    rig.dragFrom(ImVec2(grabX, rowCenterY), ImVec2(grabX + 200.0f, rowCenterY));

    const int64_t delta = static_cast<int64_t>(200.0 * kSamplesPerPixel);
    CHECK(rig.edit.clip(a, ca)->timelineStart == delta);
    // The clip on the other track moved by the same amount without being
    // touched, and kept its offset from the one that was dragged.
    CHECK(rig.edit.clip(b, cb)->timelineStart == 240000 + delta);

    // One undo entry for the whole group, not one per member.
    rig.undo.undo();
    CHECK(rig.edit.clip(a, ca)->timelineStart == 0);
    CHECK(rig.edit.clip(b, cb)->timelineStart == 240000);
}

TEST_CASE("an ungrouped clip still drags alone", "[timelinedrag][clipgroups]") {
    // The guard for the case above: without it, "the group moved" would also
    // pass if every clip on every track moved together.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    const std::string ca = rig.edit.addClip(a, clip);
    clip.timelineStart = 240000;
    const std::string cb = rig.edit.addClip(b, clip);

    const float grabX = rig.xOfSample(48000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);
    rig.dragFrom(ImVec2(grabX, rowCenterY), ImVec2(grabX + 200.0f, rowCenterY));

    CHECK(rig.edit.clip(a, ca)->timelineStart ==
          static_cast<int64_t>(200.0 * kSamplesPerPixel));
    CHECK(rig.edit.clip(b, cb)->timelineStart == 240000);
}

TEST_CASE("clicking a clip selects its range", "[timelinedrag][selection]") {
    // Clicking a clip is how you say "this one", and every range edit — group,
    // delete, loop — reads the selection. Leaving it untouched meant a clicked
    // clip was selected for some purposes and not for others.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 96000;
    clip.length = 480000;
    const std::string c = rig.edit.addClip(t, clip);

    const float grabX = rig.xOfSample(240000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    rig.view.hasSelection = false;
    rig.tick(grabX, rowCenterY, false);
    rig.tick(grabX, rowCenterY, true);

    CHECK(rig.view.selectedClipId == c);
    CHECK(rig.view.hasSelection);
    CHECK(rig.view.selectionStart == 96000);
    CHECK(rig.view.selectionEnd == 576000);
    // Scoped to the clip's own row, so a range edit cannot reach its
    // neighbours' clips.
    CHECK(rig.view.selectionRow >= 0);
    // And the cursor goes to the head of it, the same as a dragged range —
    // a selection made by clicking and one made by dragging should leave the
    // playhead in the same place.
    CHECK(rig.committedPosition() == 96000);

    rig.tick(grabX, rowCenterY, false);
}

TEST_CASE("clicking a group clip selects the group's range",
          "[timelinedrag][selection][clipgroups]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 96000;
    clip.length = 96000;
    const std::string c = rig.edit.addClip(t, clip);
    const std::string g = rig.edit.addClipGroup({{t, c, false}}, 48000, 288000,
                                                {t});
    REQUIRE_FALSE(g.empty());

    const float grabX = rig.xOfSample(240000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowCenterY > 0.0f);

    rig.view.hasSelection = false;
    rig.tick(grabX, rowCenterY, false);
    rig.tick(grabX, rowCenterY, true);

    // The group's range, not the clip's — the group is what was clicked.
    CHECK(rig.view.selectedClipId == g);
    CHECK(rig.view.selectionStart == 48000);
    CHECK(rig.view.selectionEnd == 336000);
    CHECK(rig.committedPosition() == 48000);
    rig.tick(grabX, rowCenterY, false);
}

TEST_CASE("holding Command selects a range instead of grabbing a clip",
          "[timelinedrag][selection]") {
    // Command is the selector tool: a press that lands on a clip marks a time
    // range in that lane rather than picking the clip up. Without it you can
    // only ever select a whole clip, never a span inside one. The MIDI lane
    // was the regression — its click handler ignored the modifier.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    const std::string c = rig.edit.addMidiClip(t, midiClip(96000, 480000));

    const float grabX = rig.xOfSample(240000);
    const float rowCenterY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::MidiClip);
    REQUIRE(rowCenterY > 0.0f);

    const float toX = rig.xOfSample(360000);
    // The GLFW backend reports the platform command key as ImGuiMod_Super on
    // macOS (where ImGui then swaps it to Ctrl) and ImGuiMod_Ctrl elsewhere;
    // feed whichever this build would see so selectorMode's KeyCtrl fires.
#if defined(__APPLE__)
    const ImGuiKey kCommand = ImGuiMod_Super;
#else
    const ImGuiKey kCommand = ImGuiMod_Ctrl;
#endif
    // Let ImGui settle after findRowY's probing presses: a couple of frames
    // parked off the lane with the button up clear any lingering active item
    // and drag state, so the Command gesture is judged on its own.
    rig.tick(-100.0f, -100.0f, false);
    rig.tick(-100.0f, -100.0f, false);
    rig.view.dragKind = gui::TimelineViewState::DragKind::None;
    rig.view.selectedClipId.clear();
    rig.view.isSelecting = false;
    rig.view.hasSelection = false;

    ImGui::GetIO().AddKeyEvent(kCommand, true);
    rig.dragFrom(ImVec2(grabX, rowCenterY), ImVec2(toX, rowCenterY));
    ImGui::GetIO().AddKeyEvent(kCommand, false);

    // The clip was left where it was, unselected and unmoved...
    CHECK(rig.view.selectedClipId.empty());
    CHECK_FALSE(
        rig.view.isDragging(gui::TimelineViewState::DragKind::MidiClip));
    CHECK(rig.edit.track(t)->midiClips.front().timelineStart == 96000);
    // ...and a real time range was marked in that lane instead.
    CHECK(rig.view.hasSelection);
    CHECK(rig.view.selectionRow >= 0);
    CHECK(rig.view.selectionStart != rig.view.selectionEnd);
}

TEST_CASE("a clip drags from one track onto another", "[timelinedrag]") {
    // Cross-track drag: grab a clip on the first track and release it over the
    // second, and it should belong to the second track afterwards.
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    const std::string c = rig.edit.addClip(a, clip);
    REQUIRE(rig.edit.track(a)->clips.size() == 1);
    REQUIRE(rig.edit.track(b)->clips.empty());

    const float grabX = rig.xOfSample(48000);
    const float rowAY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowAY > 0.0f);

    // Drop straight down onto the next row. One base row height (58) clears A
    // and lands inside B.
    rig.dragFrom(ImVec2(grabX, rowAY), ImVec2(grabX, rowAY + kTrackHeight));

    CHECK(rig.edit.track(a)->clips.empty());
    REQUIRE(rig.edit.track(b)->clips.size() == 1);
    // The clip keeps its identity across the move.
    CHECK(rig.edit.track(b)->clips.front().id == c);

    rig.undo.undo();
    CHECK(rig.edit.track(a)->clips.size() == 1);
    CHECK(rig.edit.track(b)->clips.empty());
}

TEST_CASE("selecting while playing leaves the playhead alone",
          "[timelinedrag][selection]") {
    // Making a selection mid-playback marks the range without snapping the
    // playhead back to its head — playback carries on from where it reached.
    engine::Transport transport;
    engine::TimeInfo time{};
    transport.seek(500000);
    transport.advanceAndFill(time, 0, 48000.0);  // apply the seek
    REQUIRE(transport.position() == 500000);

    gui::TimelineViewState view;

    SECTION("stopped: it seeks to the selection head") {
        gui::selectClipRange(view, transport, 0, 96000, 480000);
        transport.advanceAndFill(time, 0, 48000.0);
        CHECK(transport.position() == 96000);
    }
    SECTION("playing: no seek is requested") {
        transport.play();
        gui::selectClipRange(view, transport, 0, 96000, 480000);
        transport.advanceAndFill(time, 0, 48000.0);
        CHECK(transport.position() == 500000);
    }
    // Either way, the range is marked.
    CHECK(view.hasSelection);
    CHECK(view.selectionStart == 96000);
    CHECK(view.selectionEnd == 576000);
}

TEST_CASE("the selection follows a clip as it moves",
          "[timelinedrag][selection]") {
    // Clicking a clip selects its range; moving the clip has to carry that
    // range with it, or the selection is left behind over empty timeline.
    TimelineRig rig;
    rig.view.snapEnabled = false;  // assert exact samples, not a snapped grid
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    document::AudioClip clip;
    clip.timelineStart = 96000;
    clip.length = 480000;
    rig.edit.addClip(t, clip);

    const float grabX = rig.xOfSample(240000);  // mid-clip
    const float rowY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowY > 0.0f);

    // Grab at 240000, release at 480000 — a +240000-sample move.
    rig.dragFrom(ImVec2(grabX, rowY), ImVec2(rig.xOfSample(480000), rowY));

    const auto& moved = rig.edit.track(t)->clips.front();
    REQUIRE(moved.timelineStart == 336000);  // 96000 + 240000
    CHECK(rig.view.hasSelection);
    CHECK(rig.view.selectionStart == moved.timelineStart);
    CHECK(rig.view.selectionEnd == moved.timelineStart + moved.length);
}

TEST_CASE("the selection follows a clip onto another track",
          "[timelinedrag][selection]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 480000;
    rig.edit.addClip(a, clip);

    const float grabX = rig.xOfSample(48000);
    const float rowAY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowAY > 0.0f);

    // Straight down onto B (one base row height clears A).
    rig.dragFrom(ImVec2(grabX, rowAY), ImVec2(grabX, rowAY + kTrackHeight));

    REQUIRE(rig.edit.track(b)->clips.size() == 1);
    // B is the second audio track (index 1); the selection row rode along.
    CHECK(rig.view.hasSelection);
    CHECK(rig.view.selectionRow == 1);
}

TEST_CASE("a MIDI clip drags from one track onto another", "[timelinedrag]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string a = rig.edit.addMidiTrack("A");
    const std::string b = rig.edit.addMidiTrack("B");
    const std::string c = rig.edit.addMidiClip(a, midiClip(0, 480000));
    REQUIRE(rig.edit.track(a)->midiClips.size() == 1);

    const float grabX = rig.xOfSample(48000);
    const float rowAY =
        rig.findRowY(grabX, gui::TimelineViewState::DragKind::MidiClip);
    REQUIRE(rowAY > 0.0f);

    rig.dragFrom(ImVec2(grabX, rowAY), ImVec2(grabX, rowAY + kTrackHeight));

    CHECK(rig.edit.track(a)->midiClips.empty());
    REQUIRE(rig.edit.track(b)->midiClips.size() == 1);
    CHECK(rig.edit.track(b)->midiClips.front().id == c);
}

TEST_CASE("a file drop resolves to the track and sample under it",
          "[timelinedrag][drop]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    const std::string t = rig.edit.addTrack("Audio");
    // A clip so findRowY can locate the row; the drop lands elsewhere on it.
    document::AudioClip clip;
    clip.timelineStart = 0;
    clip.length = 48000;
    rig.edit.addClip(t, clip);

    // Grab mid-clip (not the edge, which would be a trim) to find the row.
    const float rowY =
        rig.findRowY(rig.xOfSample(24000),
                     gui::TimelineViewState::DragKind::AudioClip);
    REQUIRE(rowY > 0.0f);

    // Drop a WAV at 96000 samples on that row.
    const int64_t dropSample = 96000;
    rig.view.pendingFileDrops.push_back(
        {"/tmp/x.wav", rig.xOfSample(dropSample), rowY});
    // One frame resolves it.
    rig.tick(-100.0f, -100.0f, false);

    REQUIRE(rig.view.pendingFileDrops.empty());
    REQUIRE(rig.view.resolvedFileDrops.size() == 1);
    const auto& r = rig.view.resolvedFileDrops.front();
    CHECK(r.path == "/tmp/x.wav");
    CHECK(r.trackId == t);
    // Snap is off, so the sample is exactly under the cursor (± rounding).
    CHECK(std::llabs(r.sample - dropSample) <= 1);
}

TEST_CASE("a file dropped off the lanes resolves to a new track",
          "[timelinedrag][drop]") {
    TimelineRig rig;
    rig.edit.addMarkerTrack("Markers");
    rig.edit.addTrack("Audio");
    rig.tick(-100.0f, -100.0f, false);

    // Far below every row → no track; the app makes a new one.
    rig.view.pendingFileDrops.push_back({"/tmp/y.wav", 600.0f, 5000.0f});
    rig.tick(-100.0f, -100.0f, false);

    REQUIRE(rig.view.resolvedFileDrops.size() == 1);
    CHECK(rig.view.resolvedFileDrops.front().trackId.empty());
}
