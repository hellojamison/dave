// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip groups: several clips treated as one object.
//
// The clips are NOT merged. A group is a record of which clips belong
// together, held beside them — which is what makes ungrouping exact rather
// than a reconstruction, and what lets a group span tracks without inventing a
// clip that lives on more than one. Most of what follows tests that the record
// stays honest as the clips under it change.
#include "ImGuiTestRig.h"

#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "gui/Timeline.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace dave;
using Member = document::ClipGroup::Member;

namespace {

document::AudioClip audioClip(int64_t start, int64_t length) {
    document::AudioClip c;
    c.timelineStart = start;
    c.length = length;
    return c;
}

// Two tracks, two clips each, so a group can span tracks and still leave
// something outside it.
struct Fixture {
    document::Edit edit;
    editing::UndoStack undo{edit};
    std::string trackA;
    std::string trackB;
    std::string a1, a2, b1, b2;

    Fixture() {
        trackA = edit.addTrack("A");
        trackB = edit.addTrack("B");
        a1 = edit.addClip(trackA, audioClip(0, 48000));
        a2 = edit.addClip(trackA, audioClip(96000, 48000));
        b1 = edit.addClip(trackB, audioClip(0, 48000));
        b2 = edit.addClip(trackB, audioClip(96000, 48000));
    }

    int64_t startOf(const std::string& track, const std::string& clip) const {
        const auto* c = const_cast<document::Edit&>(edit).clip(track, clip);
        REQUIRE(c != nullptr);
        return c->timelineStart;
    }
};

} // namespace

// ─── The record ────────────────────────────────────────────────────────────

TEST_CASE("a group is defined by its range, not by how many clips it holds",
          "[clipgroups]") {
    // An empty group is a legitimate object — a placeholder over a section you
    // are about to fill, draggable like anything else. What has no meaning is
    // a group with no extent.
    Fixture f;
    CHECK(f.edit.addClipGroup({}, 0, 0).empty());
    CHECK(f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 0).empty());
    CHECK(f.edit.clipGroups().empty());

    // With no members and no row named there is nothing to draw it on.
    CHECK(f.edit.addClipGroup({}, 240000, 96000).empty());
    const std::string empty =
        f.edit.addClipGroup({}, 240000, 96000, {f.trackA});
    REQUIRE_FALSE(empty.empty());
    REQUIRE(f.edit.clipGroup(empty) != nullptr);
    CHECK(f.edit.clipGroup(empty)->members.empty());
    CHECK(f.edit.clipGroup(empty)->length == 96000);

    // And one clip is a perfectly good group when that is what was selected.
    CHECK_FALSE(f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 48000)
                    .empty());
}

TEST_CASE("a group can span tracks", "[clipgroups]") {
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackB, f.b1, false}}, 0, 200000);
    REQUIRE_FALSE(id.empty());
    REQUIRE(f.edit.clipGroup(id) != nullptr);
    CHECK(f.edit.clipGroup(id)->members.size() == 2);
    CHECK(f.edit.clipGroupContaining(f.trackA, f.a1) != nullptr);
    CHECK(f.edit.clipGroupContaining(f.trackB, f.b1) != nullptr);
    // The clips outside it are untouched.
    CHECK(f.edit.clipGroupContaining(f.trackA, f.a2) == nullptr);
}

TEST_CASE("a clip belongs to at most one group", "[clipgroups]") {
    // A clip in two groups would move twice for one drag.
    Fixture f;
    REQUIRE_FALSE(f.edit.addClipGroup({{f.trackA, f.a1, false},
                                       {f.trackA, f.a2, false}},
                                      0, 200000).empty());
    // a1 is taken, so the second group gets b1 only — which is allowed, but
    // it must not steal a1 from the first group.
    const std::string second = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackB, f.b1, false}}, 0, 200000);
    REQUIRE_FALSE(second.empty());
    REQUIRE(f.edit.clipGroup(second) != nullptr);
    CHECK(f.edit.clipGroup(second)->members.size() == 1);
    CHECK(f.edit.clipGroups().front().members.size() == 2);
}

TEST_CASE("a group never names a clip that has gone", "[clipgroups]") {
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackA, f.a2, false},
         {f.trackB, f.b1, false}}, 0, 200000);
    REQUIRE_FALSE(id.empty());

    REQUIRE(f.edit.removeClip(f.trackA, f.a1));
    REQUIRE(f.edit.clipGroup(id) != nullptr);
    CHECK(f.edit.clipGroup(id)->members.size() == 2);

    // Emptying a group does not delete it. The range is the object; one that
    // vanished when its last clip went would take the section with it.
    REQUIRE(f.edit.removeClip(f.trackA, f.a2));
    REQUIRE(f.edit.removeClip(f.trackB, f.b1));
    REQUIRE(f.edit.clipGroup(id) != nullptr);
    CHECK(f.edit.clipGroup(id)->members.empty());
    CHECK(f.edit.clipGroup(id)->length == 200000);
}

// ─── Moving ────────────────────────────────────────────────────────────────

TEST_CASE("moving a group moves every member by the same amount",
          "[clipgroups]") {
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackB, f.b2, false}}, 0, 200000);
    REQUIRE_FALSE(id.empty());

    f.undo.execute(std::make_unique<editing::MoveClipGroupCommand>(id, 24000));
    CHECK(f.startOf(f.trackA, f.a1) == 24000);
    CHECK(f.startOf(f.trackB, f.b2) == 120000);
    // A clip outside the group stays put.
    CHECK(f.startOf(f.trackA, f.a2) == 96000);

    f.undo.undo();
    CHECK(f.startOf(f.trackA, f.a1) == 0);
    CHECK(f.startOf(f.trackB, f.b2) == 96000);
}

TEST_CASE("a group clamps as one thing, keeping its spacing", "[clipgroups]") {
    // Clamping each clip separately would squash the members against zero and
    // lose their spacing, which is the one thing a group exists to keep.
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackA, f.a2, false}}, 0, 200000);
    REQUIRE_FALSE(id.empty());

    f.undo.execute(std::make_unique<editing::MoveClipGroupCommand>(id, -50000));
    CHECK(f.startOf(f.trackA, f.a1) == 0);
    // Still 96,000 apart, not both at zero.
    CHECK(f.startOf(f.trackA, f.a2) == 96000);

    f.undo.undo();
    CHECK(f.startOf(f.trackA, f.a1) == 0);
    CHECK(f.startOf(f.trackA, f.a2) == 96000);
}

// ─── Grouping and ungrouping ───────────────────────────────────────────────

TEST_CASE("ungrouping restores nothing because nothing was destroyed",
          "[clipgroups]") {
    Fixture f;
    auto command = std::make_unique<editing::GroupClipsCommand>(
        std::vector<Member>{{f.trackA, f.a1, false}, {f.trackA, f.a2, false}},
        0, 200000);
    auto* raw = command.get();
    f.undo.execute(std::move(command));
    const std::string id = raw->groupId();
    REQUIRE_FALSE(id.empty());

    f.undo.execute(std::make_unique<editing::UngroupClipsCommand>(id));
    CHECK(f.edit.clipGroups().empty());
    // The clips are exactly where they were the whole time.
    CHECK(f.startOf(f.trackA, f.a1) == 0);
    CHECK(f.startOf(f.trackA, f.a2) == 96000);

    f.undo.undo();
    REQUIRE(f.edit.clipGroup(id) != nullptr);
    CHECK(f.edit.clipGroup(id)->members.size() == 2);
}

TEST_CASE("a group keeps its id across redo", "[clipgroups]") {
    // Anything holding the id — a selection, a drag in flight — has to still
    // resolve after undo and redo.
    Fixture f;
    auto command = std::make_unique<editing::GroupClipsCommand>(
        std::vector<Member>{{f.trackA, f.a1, false}, {f.trackB, f.b1, false}},
        0, 200000);
    auto* raw = command.get();
    f.undo.execute(std::move(command));
    const std::string id = raw->groupId();

    f.undo.undo();
    CHECK(f.edit.clipGroups().empty());
    f.undo.redo();
    CHECK(raw->groupId() == id);
    REQUIRE(f.edit.clipGroup(id) != nullptr);
    CHECK(f.edit.clipGroup(id)->members.size() == 2);
}

TEST_CASE("groups round-trip through the project file", "[clipgroups]") {
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackB, f.b1, false}}, 24000, 120000,
        {f.trackA, f.trackB}, {}, "Chorus");
    REQUIRE_FALSE(id.empty());

    const std::string text = document::serializeEdit(f.edit);
    document::Edit reloaded;
    REQUIRE(document::deserializeEdit(text, reloaded).ok);
    REQUIRE(reloaded.clipGroups().size() == 1);
    CHECK(reloaded.clipGroups()[0].name == "Chorus");
    // The range is the group, so losing it would lose the clip you can see.
    CHECK(reloaded.clipGroups()[0].timelineStart == 24000);
    CHECK(reloaded.clipGroups()[0].length == 120000);
    CHECK(reloaded.clipGroups()[0].members.size() == 2);
    CHECK(reloaded.clipGroupContaining(f.trackA, f.a1) != nullptr);
}

// ─── What the selection covers ─────────────────────────────────────────────

TEST_CASE("the selection takes clips it overlaps, not only ones it contains",
          "[clipgroups]") {
    // A range dragged roughly around some clips is the gesture people make;
    // requiring full containment would silently drop the ones whose tails
    // stick out.
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = -1;          // a ruler drag: every track
    view.selectionStart = 24000;
    view.selectionEnd = 120000;

    const auto members = gui::clipsInSelection(f.edit, view);
    // All four: a1 and b1 end at 48,000, a2 and b2 start at 96,000.
    CHECK(members.size() == 4);
}

TEST_CASE("a selection in one lane cannot group its neighbours",
          "[clipgroups]") {
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionStart = 0;
    view.selectionEnd = 200000;
    view.selectionRow = 0;           // track A only

    const auto members = gui::clipsInSelection(f.edit, view);
    REQUIRE(members.size() == 2);
    for (const auto& member : members) CHECK(member.trackId == f.trackA);
}

TEST_CASE("no selection covers nothing", "[clipgroups]") {
    Fixture f;
    gui::TimelineViewState view;
    CHECK(gui::clipsInSelection(f.edit, view).empty());
    view.hasSelection = true;
    view.selectionStart = 5000;
    view.selectionEnd = 5000;        // an empty range is not a selection
    CHECK(gui::clipsInSelection(f.edit, view).empty());
}

TEST_CASE("grouping and ungrouping through the selection", "[clipgroups]") {
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = -1;
    view.selectionStart = 0;
    view.selectionEnd = 200000;

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    CHECK(f.edit.clipGroups()[0].members.size() == 4);

    // Everything in range is already grouped, so a second group over the same
    // selection takes no members — but it is still a group, because the
    // selection is what makes one.
    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 2);
    CHECK(f.edit.clipGroups()[1].members.empty());
    f.undo.undo();
    REQUIRE(f.edit.clipGroups().size() == 1);

    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
    CHECK_FALSE(gui::ungroupSelectedClips(f.edit, f.undo, view));
}

TEST_CASE("ungrouping a range with two groups in it clears both",
          "[clipgroups]") {
    Fixture f;
    REQUIRE_FALSE(f.edit.addClipGroup({{f.trackA, f.a1, false}, {f.trackA, f.a2, false}}, 0, 200000).empty());
    REQUIRE_FALSE(f.edit.addClipGroup({{f.trackB, f.b1, false}, {f.trackB, f.b2, false}}, 0, 200000).empty());

    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = -1;
    view.selectionStart = 0;
    view.selectionEnd = 200000;

    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
}

TEST_CASE("a group covers the selection, not its clips' extent",
          "[clipgroups]") {
    // Selecting four bars around three clips gives a four-bar object. That is
    // what was asked for, and it is what lets two groups butt up cleanly
    // instead of leaving a gap wherever the audio happened to stop.
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;
    view.selectionStart = 24000;
    view.selectionEnd = 200000;

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    const auto& group = f.edit.clipGroups()[0];
    CHECK(group.timelineStart == 24000);
    CHECK(group.length == 176000);
    // The clips inside it start at 0 and 96,000 and end at 144,000 — the
    // group is wider on the left and on the right than they are.
    CHECK(group.timelineStart > 0);
    CHECK(group.end() > 144000);
}

TEST_CASE("the group's range moves with its clips", "[clipgroups]") {
    // The range is what you see and grab. If it drifted from the members the
    // group would be a box that no longer sits over its own contents.
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackA, f.a2, false}}, 0, 200000);
    REQUIRE_FALSE(id.empty());

    f.undo.execute(std::make_unique<editing::MoveClipGroupCommand>(id, 48000));
    REQUIRE(f.edit.clipGroup(id) != nullptr);
    CHECK(f.edit.clipGroup(id)->timelineStart == 48000);
    CHECK(f.startOf(f.trackA, f.a1) == 48000);
    CHECK(f.startOf(f.trackA, f.a2) == 144000);

    f.undo.undo();
    CHECK(f.edit.clipGroup(id)->timelineStart == 0);
    CHECK(f.startOf(f.trackA, f.a1) == 0);
}

TEST_CASE("a group clamped at zero keeps its range over its clips",
          "[clipgroups]") {
    Fixture f;
    const std::string id = f.edit.addClipGroup(
        {{f.trackA, f.a1, false}, {f.trackA, f.a2, false}}, 0, 200000);
    REQUIRE_FALSE(id.empty());

    // The group already starts at zero, so this does nothing at all rather
    // than moving the range without the clips.
    f.undo.execute(std::make_unique<editing::MoveClipGroupCommand>(id, -50000));
    CHECK(f.edit.clipGroup(id)->timelineStart == 0);
    CHECK(f.startOf(f.trackA, f.a1) == 0);
    CHECK(f.startOf(f.trackA, f.a2) == 96000);

    // And undoing a move that was clamped away does not shift anything.
    f.undo.undo();
    CHECK(f.edit.clipGroup(id)->timelineStart == 0);
    CHECK(f.startOf(f.trackA, f.a2) == 96000);
}

TEST_CASE("a blank selection makes an empty group", "[clipgroups]") {
    // The selection is what makes the group, so a range with no clips in it
    // gives an empty group over that range — a placeholder you can drag now
    // and fill later.
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;
    view.selectionStart = 480000;    // past every clip in the fixture
    view.selectionEnd = 576000;

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    const auto& group = f.edit.clipGroups()[0];
    CHECK(group.members.empty());
    CHECK(group.timelineStart == 480000);
    CHECK(group.length == 96000);

    // It drags like any other group, and there is nothing to drag with it.
    f.undo.execute(std::make_unique<editing::MoveClipGroupCommand>(
        group.id, -96000));
    CHECK(f.edit.clipGroups()[0].timelineStart == 384000);
    CHECK(f.startOf(f.trackA, f.a1) == 0);

    f.undo.undo();
    CHECK(f.edit.clipGroups()[0].timelineStart == 480000);
}

TEST_CASE("a selection with no extent makes nothing", "[clipgroups]") {
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;
    view.selectionStart = 48000;
    view.selectionEnd = 48000;
    CHECK_FALSE(gui::groupSelectedClips(f.edit, f.undo, view));

    view.hasSelection = false;
    view.selectionEnd = 96000;
    CHECK_FALSE(gui::groupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
}

TEST_CASE("with no range dragged, the selected clip is the group",
          "[clipgroups]") {
    // Pressing the shortcut with a clip selected and getting nothing is the
    // same silence as pressing it with nothing selected, and only one of those
    // is a mistake.
    Fixture f;
    gui::TimelineViewState view;
    view.selectedTrackIndex = 0;
    REQUIRE(f.edit.tracks()[0].id == f.trackA);
    view.selectedClipId = f.a2;      // starts at 96,000, 48,000 long

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    const auto& group = f.edit.clipGroups()[0];
    CHECK(group.timelineStart == 96000);
    CHECK(group.length == 48000);
    REQUIRE(group.members.size() == 1);
    CHECK(group.members[0].clipId == f.a2);

    // And Cmd+Opt+U on that group clip undoes it.
    view.selectedClipId = group.id;
    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
}

TEST_CASE("with nothing selected at all, grouping does nothing",
          "[clipgroups]") {
    Fixture f;
    gui::TimelineViewState view;
    CHECK_FALSE(gui::groupSelectedClips(f.edit, f.undo, view));

    // A selected id that names no clip is the same as nothing selected.
    view.selectedTrackIndex = 0;
    view.selectedClipId = "clip_nonexistent";
    CHECK_FALSE(gui::groupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
}

TEST_CASE("a group over silence still draws somewhere", "[clipgroups]") {
    // The bug this exists for: an empty group derived its rows from its
    // members, so a group made over a blank selection covered no track and
    // drew nowhere. It looked exactly like the shortcut doing nothing.
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;
    view.selectionStart = 480000;    // past every clip in the fixture
    view.selectionEnd = 576000;

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    const auto& group = f.edit.clipGroups()[0];
    REQUIRE(group.members.empty());
    CHECK(group.coversTrack(f.trackA));
    CHECK_FALSE(group.coversTrack(f.trackB));
}

TEST_CASE("a ruler-wide selection groups across every track", "[clipgroups]") {
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = -1;          // a ruler drag
    view.selectionStart = 480000;
    view.selectionEnd = 576000;

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    const auto& group = f.edit.clipGroups()[0];
    CHECK(group.members.empty());
    CHECK(group.coversTrack(f.trackA));
    CHECK(group.coversTrack(f.trackB));
}

TEST_CASE("the rows a group spans survive a save", "[clipgroups]") {
    // Without them an empty group reloads invisible, which is the same bug
    // one restart later.
    Fixture f;
    const std::string id = f.edit.addClipGroup({}, 480000, 96000, {f.trackB});
    REQUIRE_FALSE(id.empty());

    const std::string text = document::serializeEdit(f.edit);
    document::Edit reloaded;
    REQUIRE(document::deserializeEdit(text, reloaded).ok);
    REQUIRE(reloaded.clipGroups().size() == 1);
    CHECK(reloaded.clipGroups()[0].coversTrack(f.trackB));
    CHECK_FALSE(reloaded.clipGroups()[0].coversTrack(f.trackA));
}

TEST_CASE("an empty group can be ungrouped", "[clipgroups]") {
    // Ungroup used to find groups through their members, so a group made over
    // silence could be created and dragged but never removed — the same
    // mistake that made one invisible, one step later.
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;
    view.selectionStart = 480000;
    view.selectionEnd = 576000;

    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    REQUIRE(f.edit.clipGroups()[0].members.empty());

    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());

    f.undo.undo();
    CHECK(f.edit.clipGroups().size() == 1);
}

TEST_CASE("ungroup only reaches groups the selection is over", "[clipgroups]") {
    Fixture f;
    // One group early on track A, one later on track B.
    REQUIRE_FALSE(f.edit.addClipGroup({}, 0, 96000, {f.trackA}).empty());
    REQUIRE_FALSE(f.edit.addClipGroup({}, 480000, 96000, {f.trackB}).empty());

    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;           // track A only
    view.selectionStart = 0;
    view.selectionEnd = 96000;

    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    REQUIRE(f.edit.clipGroups().size() == 1);
    // The one on the other track, out of range, is untouched.
    CHECK(f.edit.clipGroups()[0].timelineStart == 480000);

    // A selection that touches neither does nothing.
    view.selectionStart = 200000;
    view.selectionEnd = 300000;
    CHECK_FALSE(gui::ungroupSelectedClips(f.edit, f.undo, view));
}

TEST_CASE("a ruler-wide ungroup clears every group it crosses",
          "[clipgroups]") {
    Fixture f;
    REQUIRE_FALSE(f.edit.addClipGroup({}, 0, 96000, {f.trackA}).empty());
    REQUIRE_FALSE(f.edit.addClipGroup({}, 24000, 96000, {f.trackB}).empty());

    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = -1;
    view.selectionStart = 0;
    view.selectionEnd = 200000;

    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
}

// ─── Nesting ───────────────────────────────────────────────────────────────
//
// Grouping two groups gives one group containing two. Ungrouping the outer
// layer puts the inner ones back rather than flattening the whole tree, which
// is the only reason to nest in the first place.

TEST_CASE("grouping two groups nests them", "[clipgroups][nesting]") {
    Fixture f;
    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;

    view.selectionStart = 0;
    view.selectionEnd = 48000;
    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    const std::string inner1 = f.edit.clipGroups().back().id;

    view.selectionStart = 96000;
    view.selectionEnd = 144000;
    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    const std::string inner2 = f.edit.clipGroups().back().id;
    REQUIRE(inner1 != inner2);

    // Now group across both.
    view.selectionStart = 0;
    view.selectionEnd = 200000;
    REQUIRE(gui::groupSelectedClips(f.edit, f.undo, view));
    const std::string outer = f.edit.clipGroups().back().id;

    REQUIRE(f.edit.clipGroup(outer) != nullptr);
    CHECK(f.edit.clipGroup(outer)->childGroupIds.size() == 2);
    // The inner ones still exist, and are no longer the outermost thing.
    CHECK(f.edit.clipGroup(inner1) != nullptr);
    CHECK(f.edit.clipGroup(inner2) != nullptr);
    CHECK_FALSE(f.edit.clipGroupIsTopLevel(inner1));
    CHECK_FALSE(f.edit.clipGroupIsTopLevel(inner2));
    CHECK(f.edit.clipGroupIsTopLevel(outer));

    // The clips inside them did not change hands.
    CHECK(f.edit.clipGroupContaining(f.trackA, f.a1)->id == inner1);
}

TEST_CASE("ungrouping one layer restores the groups underneath",
          "[clipgroups][nesting]") {
    Fixture f;
    const std::string inner1 =
        f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 48000, {f.trackA});
    const std::string inner2 = f.edit.addClipGroup(
        {{f.trackA, f.a2, false}}, 96000, 48000, {f.trackA});
    REQUIRE_FALSE(inner1.empty());
    REQUIRE_FALSE(inner2.empty());
    const std::string outer = f.edit.addClipGroup(
        {}, 0, 200000, {f.trackA}, {inner1, inner2});
    REQUIRE_FALSE(outer.empty());

    gui::TimelineViewState view;
    view.hasSelection = true;
    view.selectionRow = 0;
    view.selectionStart = 0;
    view.selectionEnd = 200000;

    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    // One layer came off. The two underneath are back, not gone.
    CHECK(f.edit.clipGroup(outer) == nullptr);
    REQUIRE(f.edit.clipGroup(inner1) != nullptr);
    REQUIRE(f.edit.clipGroup(inner2) != nullptr);
    CHECK(f.edit.clipGroupIsTopLevel(inner1));
    CHECK(f.edit.clipGroupIsTopLevel(inner2));

    // A second ungroup peels the next layer.
    REQUIRE(gui::ungroupSelectedClips(f.edit, f.undo, view));
    CHECK(f.edit.clipGroups().empty());
}

TEST_CASE("moving an outer group carries the nested ones",
          "[clipgroups][nesting]") {
    Fixture f;
    const std::string inner =
        f.edit.addClipGroup({{f.trackA, f.a2, false}}, 96000, 48000,
                            {f.trackA});
    REQUIRE_FALSE(inner.empty());
    const std::string outer =
        f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 200000, {f.trackA},
                            {inner});
    REQUIRE_FALSE(outer.empty());

    f.undo.execute(std::make_unique<editing::MoveClipGroupCommand>(
        outer, 48000));
    CHECK(f.edit.clipGroup(outer)->timelineStart == 48000);
    // The nested group's own range moved too, or the box would stop sitting
    // over its contents.
    CHECK(f.edit.clipGroup(inner)->timelineStart == 144000);
    CHECK(f.startOf(f.trackA, f.a1) == 48000);
    CHECK(f.startOf(f.trackA, f.a2) == 144000);

    f.undo.undo();
    CHECK(f.edit.clipGroup(inner)->timelineStart == 96000);
    CHECK(f.startOf(f.trackA, f.a2) == 96000);
}

TEST_CASE("a nested group cannot be adopted twice", "[clipgroups][nesting]") {
    // A group with two parents would move twice for one drag, the same way a
    // clip in two groups would.
    Fixture f;
    const std::string inner =
        f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 48000, {f.trackA});
    const std::string first =
        f.edit.addClipGroup({}, 0, 100000, {f.trackA}, {inner});
    REQUIRE_FALSE(first.empty());

    const std::string second =
        f.edit.addClipGroup({}, 0, 200000, {f.trackA}, {inner});
    REQUIRE_FALSE(second.empty());
    CHECK(f.edit.clipGroup(second)->childGroupIds.empty());
    CHECK(f.edit.clipGroupParent(inner)->id == first);
}

TEST_CASE("removing a group frees the ones it held", "[clipgroups][nesting]") {
    Fixture f;
    const std::string inner =
        f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 48000, {f.trackA});
    const std::string outer =
        f.edit.addClipGroup({}, 0, 200000, {f.trackA}, {inner});
    REQUIRE_FALSE(outer.empty());
    REQUIRE_FALSE(f.edit.clipGroupIsTopLevel(inner));

    REQUIRE(f.edit.removeClipGroup(outer));
    // A parent naming a group that has gone would keep it hidden with nothing
    // standing in for it.
    CHECK(f.edit.clipGroupIsTopLevel(inner));
}

TEST_CASE("nesting survives a save", "[clipgroups][nesting]") {
    Fixture f;
    const std::string inner =
        f.edit.addClipGroup({{f.trackA, f.a1, false}}, 0, 48000, {f.trackA});
    const std::string outer =
        f.edit.addClipGroup({}, 0, 200000, {f.trackA}, {inner});
    REQUIRE_FALSE(outer.empty());

    const std::string text = document::serializeEdit(f.edit);
    document::Edit reloaded;
    REQUIRE(document::deserializeEdit(text, reloaded).ok);
    REQUIRE(reloaded.clipGroups().size() == 2);
    CHECK_FALSE(reloaded.clipGroupIsTopLevel(inner));
    REQUIRE(reloaded.clipGroupParent(inner) != nullptr);
    CHECK(reloaded.clipGroupParent(inner)->id == outer);
}
