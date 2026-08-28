// SPDX-License-Identifier: GPL-3.0-or-later
//
// The track list.
//
// Hiding a track from the timeline is only safe if there is somewhere it is
// still listed. The right-click menu can hide one and show them all, but
// neither says WHICH are hidden or brings back a single one — that is what
// this panel is for, so the eye toggling both ways is the whole feature.
#include "ImGuiTestRig.h"

#include "application/MainEditorLayout.h"
#include "document/Edit.h"
#include "gui/TrackList.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dave;

namespace {

struct ListRig : dave::testing::ImGuiRig {
    void tick(float x, float y, bool down) {
        frame(x, y, down, [&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(190.0f, 600.0f));
            ImGui::Begin("Tracks");
            origin = ImGui::GetCursorScreenPos();
            gui::drawTrackList(edit, view);
            ImGui::End();
        });
    }

    void clickAt(float x, float y) {
        tick(x, y, false);
        tick(x, y, true);
        tick(x, y, false);
    }

    ImVec2 origin{};
};

} // namespace

TEST_CASE("a row is where the row arithmetic says", "[tracklist]") {
    constexpr float top = 100.0f;
    CHECK(gui::trackListRowAt(top + 1.0f, top, 3) == 0);
    CHECK(gui::trackListRowAt(top + gui::kTrackListRowHeight + 1.0f, top, 3) == 1);
    CHECK(gui::trackListRowAt(top - 5.0f, top, 3) == -1);
    // Past the last row is nothing, not the last row — the space below a list
    // is not part of it.
    CHECK(gui::trackListRowAt(top + gui::kTrackListRowHeight * 9.0f, top, 3)
          == -1);
    CHECK(gui::trackListRowAt(top + 1.0f, top, 0) == -1);
}

TEST_CASE("clicking an eye asks to toggle that track", "[tracklist]") {
    ListRig rig;
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    rig.tick(-100.0f, -100.0f, false);

    // Probe down the eye column for the row that asks for track B.
    std::string asked;
    for (float y = rig.origin.y; y < rig.origin.y + 200.0f; y += 2.0f) {
        rig.view.requestToggleHiddenTrackId.clear();
        rig.clickAt(rig.origin.x + gui::kTrackListEyeWidth * 0.5f, y);
        if (rig.view.requestToggleHiddenTrackId == b) {
            asked = b;
            break;
        }
    }
    CHECK(asked == b);
    CHECK_FALSE(a.empty());
}

TEST_CASE("clicking a name selects rather than hides", "[tracklist]") {
    // The eye is a narrow column on the left; the rest of the row is the
    // track itself. Clicking a name to make it disappear would be a trap.
    ListRig rig;
    rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    rig.tick(-100.0f, -100.0f, false);

    bool selectedB = false;
    for (float y = rig.origin.y; y < rig.origin.y + 200.0f; y += 2.0f) {
        rig.view.requestToggleHiddenTrackId.clear();
        rig.view.selectedTrackIndex = -1;
        rig.clickAt(rig.origin.x + gui::kTrackListEyeWidth + 40.0f, y);
        CHECK(rig.view.requestToggleHiddenTrackId.empty());
        if (rig.view.selectedTrackIndex == 1) {
            selectedB = true;
            break;
        }
    }
    CHECK(selectedB);
    CHECK_FALSE(b.empty());
}

TEST_CASE("Main has no eye to click", "[tracklist]") {
    // The document refuses to hide it, and a control that never works is
    // worse than no control.
    ListRig rig;
    rig.edit.addTrack("A");
    rig.tick(-100.0f, -100.0f, false);

    const auto& tracks = rig.edit.tracks();
    int mainRow = -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].isMain) mainRow = static_cast<int>(i);
    }
    REQUIRE(mainRow >= 0);

    for (float y = rig.origin.y; y < rig.origin.y + 200.0f; y += 2.0f) {
        rig.view.requestToggleHiddenTrackId.clear();
        rig.clickAt(rig.origin.x + gui::kTrackListEyeWidth * 0.5f, y);
        CHECK(rig.view.requestToggleHiddenTrackId !=
              std::string(document::kMainBusId));
    }
}

TEST_CASE("a hidden track is still listed", "[tracklist]") {
    // The entire point: a track you cannot see on the timeline has to be
    // somewhere you can find it and click it back.
    ListRig rig;
    const std::string a = rig.edit.addTrack("A");
    REQUIRE(rig.edit.setTrackHidden(a, true));
    rig.tick(-100.0f, -100.0f, false);

    bool foundHidden = false;
    for (float y = rig.origin.y; y < rig.origin.y + 200.0f; y += 2.0f) {
        rig.view.requestToggleHiddenTrackId.clear();
        rig.clickAt(rig.origin.x + gui::kTrackListEyeWidth * 0.5f, y);
        if (rig.view.requestToggleHiddenTrackId == a) {
            foundHidden = true;
            break;
        }
    }
    CHECK(foundHidden);
}

TEST_CASE("a closed track list costs no width at all", "[tracklist]") {
    using dave::application::calculateMainEditorLayout;
    const auto closed = calculateMainEditorLayout(
        1600.0f, 900.0f, 6.0f, 260.0f, 200.0f, false, 320.0f, 120.0f, true,
        190.0f, false);
    CHECK(closed.leftPanelWidth == 0.0f);
    // The sidebar's splitter is the only one reserved.
    CHECK(closed.editorWidth == 1600.0f - 6.0f - 260.0f);

    const auto open = calculateMainEditorLayout(
        1600.0f, 900.0f, 6.0f, 260.0f, 200.0f, false, 320.0f, 120.0f, true,
        190.0f, true);
    CHECK(open.leftPanelWidth == 190.0f);
    CHECK(open.editorWidth == 1600.0f - 12.0f - 260.0f - 190.0f);
}

TEST_CASE("both panels together cannot squeeze the editor away",
          "[tracklist]") {
    using dave::application::calculateMainEditorLayout;
    // A narrow window with both panels asking for more than fits.
    const auto layout = calculateMainEditorLayout(
        700.0f, 900.0f, 6.0f, 400.0f, 200.0f, false, 320.0f, 120.0f, true,
        400.0f, true);
    CHECK(layout.editorWidth >= 320.0f);
    CHECK(layout.leftPanelWidth + layout.sidebarWidth + layout.editorWidth ==
          700.0f - 12.0f);
}

TEST_CASE("shift-clicking selects a contiguous range of tracks", "[tracklist]") {
    ListRig rig;
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");
    const std::string c = rig.edit.addTrack("C");
    const std::string d = rig.edit.addTrack("D");
    rig.tick(-100.0f, -100.0f, false);

    const float x = rig.origin.x + gui::kTrackListEyeWidth + 40.0f;
    // Row Y for a given index, from the same arithmetic the list draws with.
    // The header line + its 2px pad sit above row 0, so probe to find row 0.
    auto rowY = [&](int idx) {
        // find row 0 by sweeping, then step by row height.
        for (float y = rig.origin.y; y < rig.origin.y + 120.0f; y += 1.0f) {
            rig.view.selectedTrackIndex = -1;
            rig.clickAt(x, y);
            if (rig.view.selectedTrackIndex == 0) {
                return y + idx * gui::kTrackListRowHeight;
            }
        }
        return -1.0f;
    };
    const float y0 = rowY(0);
    REQUIRE(y0 > 0.0f);

    // Plain-click row 1 (B): single selection, and the anchor.
    rig.clickAt(x, y0 + gui::kTrackListRowHeight);
    CHECK(rig.view.selectedTrackIndex == 1);
    CHECK(rig.view.selectedTrackIds.size() == 1);
    CHECK(rig.view.selectedTrackIds.count(b) == 1);

    // Shift-click row 3 (D): B..D selected (B, C, D).
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    rig.clickAt(x, y0 + gui::kTrackListRowHeight * 3.0f);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);

    CHECK(rig.view.selectedTrackIds.size() == 3);
    CHECK(rig.view.selectedTrackIds.count(b) == 1);
    CHECK(rig.view.selectedTrackIds.count(c) == 1);
    CHECK(rig.view.selectedTrackIds.count(d) == 1);
    CHECK(rig.view.selectedTrackIds.count(a) == 0);
    CHECK(rig.view.selectedTrackIndex == 3);   // clicked row is primary

    // A plain click collapses back to one.
    rig.clickAt(x, y0);
    CHECK(rig.view.selectedTrackIds.size() == 1);
    CHECK(rig.view.selectedTrackIds.count(a) == 1);
}
