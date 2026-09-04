// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists: alternate sets of clips a track can switch between. The active
// playlist IS the track's clips; the others are parked in the roster. What
// matters is that switching swaps content without losing or duplicating a
// clip, that a duplicate gets its own ids, that the active one cannot be
// deleted, that it all undoes, and that it survives a save.
#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace dave;

namespace {
document::AudioClip clipAt(int64_t start) {
    document::AudioClip c;
    c.timelineStart = start;
    c.length = 48000;
    return c;
}
}  // namespace

TEST_CASE("switching playlists swaps the track's clips in and out",
          "[playlists]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Vox");
    const std::string a1 = edit.addClip(t, clipAt(0));

    // A new empty playlist does not change what plays.
    const std::string second = edit.addPlaylist(t, "", false);
    REQUIRE_FALSE(second.empty());
    CHECK(edit.track(t)->clips.size() == 1);
    // Materialising the roster named the original.
    REQUIRE(edit.track(t)->playlists.size() == 2);
    CHECK(edit.track(t)->playlists.front().name == "Vox.01");
    CHECK(edit.track(t)->playlists.back().name == "Vox.02");

    // Switch: the take parks, the new one (empty) is live.
    REQUIRE(edit.switchPlaylist(t, second));
    CHECK(edit.track(t)->clips.empty());
    CHECK(edit.clip(t, a1) == nullptr);
    const std::string b1 = edit.addClip(t, clipAt(96000));

    // And back: the original take returns, the second parks with its clip.
    const std::string first = edit.track(t)->playlists.front().id;
    REQUIRE(edit.switchPlaylist(t, first));
    REQUIRE(edit.clip(t, a1) != nullptr);
    CHECK(edit.clip(t, b1) == nullptr);
    CHECK(edit.playlist(t, second)->clips.size() == 1);
    CHECK(edit.playlist(t, second)->clips.front().id == b1);
    // Switching to the active one is a no-op.
    CHECK_FALSE(edit.switchPlaylist(t, first));
}

TEST_CASE("a duplicated playlist copies the clips under fresh ids",
          "[playlists]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Gtr");
    const std::string a1 = edit.addClip(t, clipAt(1000));
    const std::string dup = edit.addPlaylist(t, "Alt", true);
    const auto* parked = edit.playlist(t, dup);
    REQUIRE(parked != nullptr);
    REQUIRE(parked->clips.size() == 1);
    CHECK(parked->clips.front().timelineStart == 1000);
    CHECK(parked->clips.front().id != a1);
    // The live clip is untouched.
    CHECK(edit.clip(t, a1) != nullptr);
}

TEST_CASE("the active playlist cannot be deleted; others can, undoably",
          "[playlists]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Keys");
    edit.addClip(t, clipAt(0));

    auto add = std::make_unique<editing::AddPlaylistCommand>(t, "", true);
    auto* addRaw = add.get();
    undo.execute(std::move(add));
    const std::string second = addRaw->playlistId();
    REQUIRE(edit.playlist(t, second) != nullptr);
    const std::string first = edit.track(t)->activePlaylistId;
    CHECK_FALSE(edit.removePlaylist(t, first));

    undo.execute(std::make_unique<editing::SwitchPlaylistCommand>(t, second));
    CHECK(edit.track(t)->activePlaylistId == second);
    undo.undo();
    CHECK(edit.track(t)->activePlaylistId == first);
    CHECK(edit.track(t)->clips.size() == 1);

    undo.execute(std::make_unique<editing::RemovePlaylistCommand>(t, second));
    CHECK(edit.playlist(t, second) == nullptr);
    undo.undo();
    REQUIRE(edit.playlist(t, second) != nullptr);
    CHECK(edit.playlist(t, second)->clips.size() == 1);

    // Executing the remove after undoing the switch dropped the switch from
    // the stack, so the stack is now [add, remove(undone)]: the next undo is
    // the add itself, and redo brings the playlist back under the same id.
    undo.undo();
    CHECK(edit.playlist(t, second) == nullptr);
    undo.redo();
    CHECK(edit.playlist(t, second) != nullptr);
}

TEST_CASE("playlists round-trip through the project file", "[playlists]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Bass");
    edit.addClip(t, clipAt(0));
    const std::string second = edit.addPlaylist(t, "Take 2", true);
    REQUIRE(edit.switchPlaylist(t, second));

    const std::string text = document::serializeEdit(edit);
    document::Edit loaded;
    REQUIRE(document::deserializeEdit(text, loaded).ok);
    const auto* track = loaded.track(t);
    REQUIRE(track != nullptr);
    CHECK(track->activePlaylistId == second);
    REQUIRE(track->playlists.size() == 2);
    // The live clips are the duplicate's; the original is parked with one.
    CHECK(track->clips.size() == 1);
    CHECK(loaded.playlist(t, track->playlists.front().id)->clips.size() == 1);
    CHECK(loaded.playlist(t, second)->clips.empty());
    CHECK(loaded.playlist(t, second)->name == "Take 2");
}
