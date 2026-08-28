// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side rails.
//
// A panel that can be closed needs a permanent place to be reopened from. The
// rails are that place, so what they must be is always the same width and
// always hit-testable — a rail that moved or disappeared with its panel would
// be no better than the toolbar button it replaced.
#include "ImGuiTestRig.h"

#include "application/MainEditorLayout.h"
#include "gui/SideRail.h"

#include <catch2/catch_test_macros.hpp>

using dave::application::calculateMainEditorLayout;

TEST_CASE("rail buttons stack without overlapping", "[siderail]") {
    const float first = dave::gui::sideRailButtonY(100.0f, 0);
    const float second = dave::gui::sideRailButtonY(100.0f, 1);
    CHECK(first > 100.0f);
    // The next button starts past the bottom of the one before it, or two
    // adjacent panels would share a hit box.
    CHECK(second >= first + dave::gui::kSideRailButtonSize);
    CHECK(second - first ==
          dave::gui::kSideRailButtonSize + dave::gui::kSideRailButtonGap);
}

TEST_CASE("a rail button fits inside the rail", "[siderail]") {
    // A button wider than the rail would spill into the panel beside it.
    CHECK(dave::gui::kSideRailButtonSize < dave::gui::kSideRailWidth);
}

TEST_CASE("the rails leave the editor its minimum width", "[siderail]") {
    // The layout below the rails is given the space between them, so a narrow
    // window squeezes the panels rather than the arrangement editor.
    constexpr float rails = dave::gui::kSideRailWidth * 2.0f;
    const float inner = 700.0f - rails;
    const auto layout = calculateMainEditorLayout(
        inner, 900.0f, 6.0f, 400.0f, 200.0f, false, 320.0f, 120.0f, true,
        400.0f, true);
    CHECK(layout.editorWidth >= 320.0f);
    CHECK(layout.leftPanelWidth + layout.sidebarWidth + layout.editorWidth ==
          inner - 12.0f);
}

TEST_CASE("clicking a rail button reports it", "[siderail]") {
    // Drawn straight into the rig's own window rather than a nested one: the
    // thing under test is the button's hit box, and a second window only adds
    // a way for the test to be wrong about which one the mouse is over.
    struct Rig : dave::testing::ImGuiRig {
        void tick(float x, float y, bool down) {
            frame(x, y, down, [&] {
                buttonPos = ImGui::GetCursorScreenPos();
                clicked = dave::gui::sideRailButton("test", buttonPos, active,
                                                    "Tooltip");
            });
        }
        bool active = false;
        bool clicked = false;
        ImVec2 buttonPos{};
    };

    Rig rig;
    rig.tick(-100.0f, -100.0f, false);
    const ImVec2 centre(
        rig.buttonPos.x + dave::gui::kSideRailButtonSize * 0.5f,
        rig.buttonPos.y + dave::gui::kSideRailButtonSize * 0.5f);

    rig.clicked = false;
    rig.tick(centre.x, centre.y, false);
    rig.tick(centre.x, centre.y, true);
    CHECK(rig.clicked);

    // A click below the button is not that button — the rail is full of them
    // and each has to own only its own square.
    rig.clicked = false;
    rig.tick(centre.x, centre.y + dave::gui::kSideRailButtonSize * 3.0f, false);
    rig.tick(centre.x, centre.y + dave::gui::kSideRailButtonSize * 3.0f, true);
    CHECK_FALSE(rig.clicked);
}
