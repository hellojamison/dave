// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/MainEditorLayout.h"

#include <catch2/catch_test_macros.hpp>

using dave::application::calculateMainEditorLayout;

TEST_CASE("window resizing changes only the main editor allocation",
          "[layout][window-resize]") {
    const auto normal = calculateMainEditorLayout(
        1440.0f, 852.0f, 6.0f, 360.0f, 340.0f, true);
    const auto minimum = calculateMainEditorLayout(
        900.0f, 492.0f, 6.0f, 360.0f, 340.0f, true);

    CHECK(normal.sidebarWidth == 360.0f);
    CHECK(minimum.sidebarWidth == normal.sidebarWidth);
    CHECK(normal.mixerHeight == 340.0f);
    CHECK(minimum.mixerHeight == normal.mixerHeight);

    CHECK(normal.editorWidth > minimum.editorWidth);
    CHECK(normal.timelineHeight > minimum.timelineHeight);
}

TEST_CASE("temporary safety clamps do not produce negative editor geometry",
          "[layout][window-resize]") {
    const auto constrained = calculateMainEditorLayout(
        400.0f, 180.0f, 6.0f, 360.0f, 340.0f, true);

    CHECK(constrained.editorWidth >= 320.0f);
    CHECK(constrained.timelineHeight >= 120.0f);
    CHECK(constrained.sidebarWidth >= 0.0f);
    CHECK(constrained.mixerHeight >= 0.0f);
}
