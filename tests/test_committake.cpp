// SPDX-License-Identifier: GPL-3.0-or-later
#include "editing/Commands.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("committed take keeps its clip identity across undo and redo",
          "[recording][committake]") {
    dave::document::Edit edit;
    const std::string trackId = edit.addTrack("Dialogue");

    dave::document::AudioAsset asset;
    asset.id = dave::document::AssetId{"finished-take-sha"};
    asset.path = "/session/recordings/Dialogue.wav";
    asset.sampleRate = 48000;
    asset.channels = 1;
    asset.lengthSamples = 4800;

    dave::document::AudioClip clip;
    clip.asset = asset.id;
    clip.timelineStart = 1200;
    clip.length = asset.lengthSamples;

    dave::editing::UndoStack undo(edit);
    undo.execute(std::make_unique<dave::editing::CommitTakeCommand>(
        trackId, asset, clip));
    REQUIRE(edit.track(trackId)->clips.size() == 1);
    const std::string stableId = edit.track(trackId)->clips.front().id;
    REQUIRE_FALSE(stableId.empty());
    REQUIRE(edit.asset(asset.id) != nullptr);

    undo.undo();
    CHECK(edit.track(trackId)->clips.empty());
    CHECK(edit.asset(asset.id) != nullptr);

    undo.redo();
    REQUIRE(edit.track(trackId)->clips.size() == 1);
    CHECK(edit.track(trackId)->clips.front().id == stableId);
    CHECK(edit.track(trackId)->clips.front().timelineStart == 1200);
    CHECK(edit.track(trackId)->clips.front().length == 4800);
}
