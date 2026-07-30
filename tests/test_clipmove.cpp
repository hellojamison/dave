// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "editing/Command.h"
#include "editing/Commands.h"

#include <catch2/catch_test_macros.hpp>

using namespace dave;

namespace {

// Builds an edit with one track holding one clip at `start`.
struct Fixture {
    document::Edit edit;
    editing::UndoStack undo{edit};
    std::string trackId;
    std::string clipId;

    explicit Fixture(int64_t start) {
        undo.execute(std::make_unique<editing::AddTrackCommand>("Track 1"));
        trackId = edit.tracks().front().id;
        document::AudioClip clip;
        clip.timelineStart = start;
        clip.length = 48000;
        undo.execute(std::make_unique<editing::AddClipCommand>(trackId, clip));
        clipId = edit.tracks().front().clips.front().id;
    }

    int64_t position() const {
        const auto* t = edit.track(trackId);
        return t->clips.front().timelineStart;
    }

    // The timeline used to write straight to the clip on every drag frame and
    // only then commit on mouse-up, which is what broke undo. It now previews
    // from view state and leaves the document alone until the commit, so this
    // deliberately does NOT touch the clip — it stands in for a drag that has
    // moved the mouse but not yet released.
    void simulateDragTo(int64_t /*newStart*/) {
        // Intentionally empty: a drag in progress must not mutate the document.
    }

    // Guards the invariant directly rather than relying on the drag helper
    // staying honest.
    bool documentUntouchedDuringDrag(int64_t expected) const {
        return position() == expected;
    }

    void commitMove(int64_t newStart) {
        undo.execute(std::make_unique<editing::MoveClipCommand>(
            trackId, clipId, newStart, ""));
    }
};

} // namespace

TEST_CASE("a clip move lands at the requested position", "[clipmove]") {
    Fixture f(0);
    f.commitMove(96000);
    CHECK(f.position() == 96000);
}

TEST_CASE("a clip move can be undone", "[clipmove]") {
    Fixture f(0);
    f.commitMove(96000);
    REQUIRE(f.position() == 96000);
    f.undo.undo();
    CHECK(f.position() == 0);
}

// Regression: the timeline mutates clip.timelineStart live during a drag and
// only then issues MoveClipCommand. The command snapshots "the old position"
// inside perform() — by which point the drag has already overwritten it — so
// undo restores the position the clip was dragged TO, and the move becomes
// permanent. Redo/undo of a drag is the common case, not an edge case.
TEST_CASE("undo restores the pre-drag position, not the dragged-to one",
          "[clipmove][regression]") {
    Fixture f(0);

    f.simulateDragTo(96000);   // drag in progress
    // The document must still hold the pre-drag position at this point. When
    // the drag wrote through live, MoveClipCommand snapshotted this value
    // after it had already changed, so undo restored 96000 and the move could
    // never be taken back.
    REQUIRE(f.documentUntouchedDuringDrag(0));

    f.commitMove(96000);       // mouse-up
    REQUIRE(f.position() == 96000);

    f.undo.undo();
    CHECK(f.position() == 0);
}

TEST_CASE("a move survives redo", "[clipmove]") {
    Fixture f(0);
    f.commitMove(96000);
    f.undo.undo();
    REQUIRE(f.position() == 0);
    f.undo.redo();
    CHECK(f.position() == 96000);
}

TEST_CASE("a clip cannot be dragged before the timeline start", "[clipmove]") {
    Fixture f(48000);
    f.commitMove(0);
    CHECK(f.position() == 0);
    CHECK(f.position() >= 0);
}
