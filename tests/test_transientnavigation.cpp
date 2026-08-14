// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TransientNavigation.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace dave;

namespace {

document::AudioClip clip(const char* asset, int64_t timeline,
                         int64_t source, int64_t length) {
    document::AudioClip value;
    value.asset.sha256 = asset;
    value.timelineStart = timeline;
    value.sourceOffset = source;
    value.length = length;
    return value;
}

audio::TransientAnalysisCache::Snapshot ready(
    std::initializer_list<audio::TransientCandidate> candidates) {
    return {audio::TransientAnalysisCache::Status::Ready,
            std::make_shared<const std::vector<audio::TransientCandidate>>(
                candidates)};
}

} // namespace

TEST_CASE("clip-boundary landmarks are sorted and deduplicated",
          "[transient][navigation]") {
    document::Track track;
    track.clips = {clip("a", 100, 0, 100), clip("b", 200, 0, 50),
                   clip("ignored", 400, 0, 0)};
    const auto result = gui::collectTrackLandmarks(
        track, gui::TimelineLandmarkMode::ClipBoundaries, {}, 50);
    REQUIRE(result.samples == std::vector<int64_t>{100, 200, 250});
    REQUIRE_FALSE(result.analysisPending);
}

TEST_CASE("transients map through clip trims and timeline placement",
          "[transient][navigation]") {
    document::Track track;
    track.clips = {clip("asset", 1000, 200, 300),
                   clip("asset", 2000, 300, 200)};
    gui::TransientSnapshotMap analyses;
    analyses.emplace("asset", ready({{150, 1.0f}, {200, 0.9f},
                                      {350, 0.8f}, {499, 0.7f},
                                      {500, 1.0f}}));
    const auto result = gui::collectTrackLandmarks(
        track, gui::TimelineLandmarkMode::Transients, analyses, 100);
    REQUIRE(result.samples ==
            std::vector<int64_t>{1000, 1150, 1299, 2050, 2199});
}

TEST_CASE("pending and failed asset analyses remain visible to the caller",
          "[transient][navigation]") {
    document::Track track;
    track.clips = {clip("pending", 0, 0, 100),
                   clip("failed", 100, 0, 100)};
    gui::TransientSnapshotMap analyses;
    analyses.emplace("pending", audio::TransientAnalysisCache::Snapshot{
        audio::TransientAnalysisCache::Status::Pending, nullptr});
    analyses.emplace("failed", audio::TransientAnalysisCache::Snapshot{
        audio::TransientAnalysisCache::Status::Failed, nullptr});
    const auto result = gui::collectTrackLandmarks(
        track, gui::TimelineLandmarkMode::Transients, analyses, 50);
    REQUIRE(result.analysisPending);
    REQUIRE(result.analysisFailed);
    REQUIRE(result.samples.empty());
}

TEST_CASE("landmark search is exclusive in both directions",
          "[transient][navigation]") {
    const std::vector<int64_t> landmarks{100, 200, 300};
    int64_t destination = 0;
    REQUIRE(gui::findTimelineLandmark(
        landmarks, 200, gui::NavigationDirection::Next, destination));
    REQUIRE(destination == 300);
    REQUIRE(gui::findTimelineLandmark(
        landmarks, 200, gui::NavigationDirection::Previous, destination));
    REQUIRE(destination == 100);
    REQUIRE_FALSE(gui::findTimelineLandmark(
        landmarks, 300, gui::NavigationDirection::Next, destination));
    REQUIRE_FALSE(gui::findTimelineLandmark(
        landmarks, 100, gui::NavigationDirection::Previous, destination));
}

TEST_CASE("selection focus contracts through and crosses its anchor",
          "[transient][navigation]") {
    auto selection = gui::applyTimelineNavigation({}, 100, 300, true);
    REQUIRE(selection.active);
    REQUIRE(selection.anchor == 100);
    REQUIRE(selection.focus == 300);
    REQUIRE(selection.start == 100);
    REQUIRE(selection.end == 300);

    selection = gui::applyTimelineNavigation(selection, 300, 50, true);
    REQUIRE(selection.anchor == 100);
    REQUIRE(selection.focus == 50);
    REQUIRE(selection.start == 50);
    REQUIRE(selection.end == 100);

    REQUIRE_FALSE(gui::applyTimelineNavigation(selection, 50, 400, false).active);
}

TEST_CASE("transient shortcut chords match macOS and Windows conventions",
          "[transient][navigation]") {
    REQUIRE(gui::transientNavigationShortcut(
                true, gui::NavigationDirection::Next, false) == ImGuiKey_Tab);
    REQUIRE(gui::transientNavigationShortcut(
                true, gui::NavigationDirection::Previous, false) ==
            (ImGuiMod_Alt | ImGuiKey_Tab));
    REQUIRE(gui::transientNavigationShortcut(
                false, gui::NavigationDirection::Previous, true) ==
            (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab));
    REQUIRE(gui::transientNavigationToggleShortcut(true) ==
            (ImGuiMod_Super | ImGuiMod_Alt | ImGuiKey_Tab));
    REQUIRE(gui::transientNavigationToggleShortcut(false) ==
            (ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_T));
}

TEST_CASE("dense transient ticks keep the strongest visible event",
          "[transient][navigation]") {
    const auto ticks = gui::cullTransientTicks(
        {{20.0f, 0.4f}, {10.0f, 0.3f}, {11.5f, 0.9f},
         {23.0f, 0.5f}, {30.0f, 0.2f}});
    REQUIRE(ticks == std::vector<gui::TransientTickPosition>{
        {11.5f, 0.9f}, {20.0f, 0.4f}, {23.0f, 0.5f}, {30.0f, 0.2f}});
}
