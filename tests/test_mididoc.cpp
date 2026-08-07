// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Command.h"
#include "editing/Commands.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace dave;

namespace {

document::MidiNote note(int64_t start, int64_t len, uint8_t pitch,
                        uint8_t vel = 100, uint8_t chan = 0) {
    document::MidiNote n;
    n.startSample = start;
    n.lengthSamples = len;
    n.pitch = pitch;
    n.velocity = vel;
    n.channel = chan;
    return n;
}

document::MidiClip clipWithNotes(int64_t timelineStart, int64_t length) {
    document::MidiClip c;
    c.name = "Bar 1";
    c.timelineStart = timelineStart;
    c.length = length;
    c.notes = {note(0, 12000, 60), note(12000, 12000, 64), note(24000, 24000, 67)};
    c.sourcePath = "/tmp/fixture.mid";
    c.sourcePpq = 960;
    c.sourceTempi = {{0, 500000}, {1920, 250000}};
    return c;
}

document::PluginSlot instrumentSlot() {
    document::PluginSlot s;
    s.name = "Surge XT";
    s.uidString = "0123456789ABCDEF0123456789ABCDEF";
    s.path = "/Library/Audio/Plug-Ins/VST3/Surge XT.vst3";
    s.stateBase64 = "AAECAwQ=";
    return s;
}

// Equality by value, not by pointer: these comparisons are what "the project
// round-tripped" actually means.
bool sameNotes(const std::vector<document::MidiNote>& a,
               const std::vector<document::MidiNote>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].startSample != b[i].startSample) return false;
        if (a[i].lengthSamples != b[i].lengthSamples) return false;
        if (a[i].pitch != b[i].pitch) return false;
        if (a[i].velocity != b[i].velocity) return false;
        if (a[i].channel != b[i].channel) return false;
    }
    return true;
}

bool sameSlot(const document::PluginSlot& a, const document::PluginSlot& b) {
    return a.id == b.id && a.name == b.name && a.uidString == b.uidString &&
           a.path == b.path && a.bypass == b.bypass &&
           a.stateBase64 == b.stateBase64;
}

// A compact description of the whole MIDI side of an Edit, so a test can assert
// "nothing changed" without spelling out every field at every call site.
std::string fingerprint(const document::Edit& e) {
    std::string s;
    for (const auto& mt : e.midiTracks()) {
        s += mt.id + "|" + mt.name + "|" + std::to_string(mt.gain) + "|" +
             std::to_string(mt.pan) + "|" + (mt.mute ? "M" : "-") +
             (mt.solo ? "S" : "-") + "|" + mt.instrument.id + ":" +
             mt.instrument.uidString + "|";
        for (const auto& c : mt.clips) {
            s += c.id + "@" + std::to_string(c.timelineStart) + "+" +
                 std::to_string(c.sourceOffset) + "/" + std::to_string(c.length) +
                 "#" + std::to_string(c.notes.size()) + ";";
        }
        for (const auto& p : mt.plugins) s += "fx:" + p.id + ";";
        s += "\n";
    }
    return s;
}

} // namespace

// ─── Edit accessors ─────────────────────────────────────────────────────────

TEST_CASE("a MIDI track can be added, found, and removed", "[mididoc]") {
    document::Edit e;
    const std::string id = e.addMidiTrack("Strings");
    REQUIRE(e.midiTracks().size() == 1);
    REQUIRE(e.midiTrack(id) != nullptr);
    CHECK(e.midiTrack(id)->name == "Strings");
    CHECK(e.removeMidiTrack(id));
    CHECK(e.midiTracks().empty());
    CHECK(e.midiTrack(id) == nullptr);
}

TEST_CASE("MIDI clips get unique ids and can be removed", "[mididoc]") {
    document::Edit e;
    const std::string t = e.addMidiTrack("Keys");
    const std::string a = e.addMidiClip(t, clipWithNotes(0, 48000));
    const std::string b = e.addMidiClip(t, clipWithNotes(48000, 48000));
    CHECK_FALSE(a.empty());
    CHECK(a != b);
    REQUIRE(e.midiClip(t, a) != nullptr);
    CHECK(e.midiClip(t, a)->notes.size() == 3);
    CHECK(e.removeMidiClip(t, a));
    CHECK(e.midiTrack(t)->clips.size() == 1);
}

TEST_CASE("adding a clip to a missing track fails instead of crashing",
          "[mididoc]") {
    document::Edit e;
    CHECK(e.addMidiClip("nope", clipWithNotes(0, 1000)).empty());
    CHECK_FALSE(e.removeMidiClip("nope", "alsonope"));
    CHECK_FALSE(e.setMidiInstrument("nope", instrumentSlot()));
}

TEST_CASE("an instrument slot gets an id only when it names a plugin",
          "[mididoc]") {
    document::Edit e;
    const std::string t = e.addMidiTrack("Keys");
    // A default-constructed slot means "no instrument" and must not be given
    // an id — an id with no uid would look like a live slot to the GraphBuilder.
    CHECK(e.setMidiInstrument(t, document::PluginSlot{}));
    CHECK(e.midiTrack(t)->instrument.id.empty());

    CHECK(e.setMidiInstrument(t, instrumentSlot()));
    CHECK_FALSE(e.midiTrack(t)->instrument.id.empty());
    CHECK(e.midiTrack(t)->instrument.name == "Surge XT");
}

TEST_CASE("content end spans MIDI clips as well as audio ones", "[mididoc]") {
    document::Edit e;
    const std::string t = e.addMidiTrack("Keys");
    e.addMidiClip(t, clipWithNotes(96000, 48000));
    CHECK(e.contentEndSamples() >= 144000);
}

TEST_CASE("solo on a MIDI track is visible to the global solo scan",
          "[mididoc][solo]") {
    document::Edit e;
    const std::string audio = e.addTrack("Audio");
    const std::string midi = e.addMidiTrack("Keys");
    CHECK_FALSE(e.anySoloed());

    e.midiTrack(midi)->solo = true;
    // The whole point of the scan living on the Edit: soloing a MIDI track has
    // to silence AUDIO tracks too, which only works if both lists are scanned.
    CHECK(e.anySoloed());
    CHECK_FALSE(document::trackAudible(*e.track(audio), e.anySoloed()));
    CHECK(document::trackAudible(*e.midiTrack(midi), e.anySoloed()));
}

// ─── Persistence ────────────────────────────────────────────────────────────

TEST_CASE("a MIDI track round-trips through JSON", "[mididoc][json]") {
    document::Edit e;
    const std::string t = e.addMidiTrack("Keys");
    e.midiTrack(t)->gain = 0.75;
    e.midiTrack(t)->pan = -0.25;
    e.midiTrack(t)->mute = true;
    e.setMidiInstrument(t, instrumentSlot());
    document::PluginSlot fx;
    fx.name = "Pro-Q";
    fx.uidString = "FEDCBA9876543210FEDCBA9876543210";
    e.addMidiPlugin(t, fx);
    e.addMidiClip(t, clipWithNotes(24000, 48000));

    document::Edit loaded;
    auto r = document::deserializeEdit(document::serializeEdit(e), loaded);
    REQUIRE(r.ok);

    REQUIRE(loaded.midiTracks().size() == 1);
    const auto& before = e.midiTracks()[0];
    const auto& after = loaded.midiTracks()[0];
    CHECK(after.id == before.id);
    CHECK(after.name == before.name);
    CHECK(after.gain == before.gain);
    CHECK(after.pan == before.pan);
    CHECK(after.mute == before.mute);
    CHECK(after.solo == before.solo);
    CHECK(sameSlot(after.instrument, before.instrument));
    REQUIRE(after.plugins.size() == 1);
    CHECK(sameSlot(after.plugins[0], before.plugins[0]));

    REQUIRE(after.clips.size() == 1);
    CHECK(after.clips[0].id == before.clips[0].id);
    CHECK(after.clips[0].name == before.clips[0].name);
    CHECK(after.clips[0].timelineStart == before.clips[0].timelineStart);
    CHECK(after.clips[0].sourceOffset == before.clips[0].sourceOffset);
    CHECK(after.clips[0].length == before.clips[0].length);
    CHECK(sameNotes(after.clips[0].notes, before.clips[0].notes));
}

TEST_CASE("clip provenance survives the round trip", "[mididoc][json]") {
    // sourcePpq/sourceTempi are dormant today. They are only worth storing if
    // they actually come back, so assert that now rather than discovering in a
    // later milestone that every saved project lost its tempo map.
    document::Edit e;
    const std::string t = e.addMidiTrack("Keys");
    e.addMidiClip(t, clipWithNotes(0, 48000));

    document::Edit loaded;
    REQUIRE(document::deserializeEdit(document::serializeEdit(e), loaded).ok);

    const auto& c = loaded.midiTracks()[0].clips[0];
    CHECK(c.sourcePath == "/tmp/fixture.mid");
    CHECK(c.sourcePpq == 960);
    REQUIRE(c.sourceTempi.size() == 2);
    CHECK(c.sourceTempi[1].tick == 1920);
    CHECK(c.sourceTempi[1].microsecondsPerQuarter == 250000);
}

TEST_CASE("a project written before MIDI existed still loads", "[mididoc][json]") {
    // The "midiTracks" key is additive and contains()-guarded; a v1 document
    // without it must load as a project with no MIDI, not fail.
    const std::string legacy = R"({
        "format": "dave.doc/v1",
        "sampleRate": 48000,
        "tracks": [{"id": "track_1", "name": "Audio 1", "clips": [], "plugins": []}],
        "assets": [],
        "markerTracks": [],
        "videoTracks": []
    })";
    document::Edit e;
    auto r = document::deserializeEdit(legacy, e);
    REQUIRE(r.ok);
    CHECK(e.tracks().size() == 1);
    CHECK(e.midiTracks().empty());
}

TEST_CASE("loading replaces MIDI tracks rather than appending to them",
          "[mididoc][json]") {
    document::Edit e;
    e.addMidiTrack("Stale");
    const std::string legacy = R"({"format":"dave.doc/v1","midiTracks":[]})";
    REQUIRE(document::deserializeEdit(legacy, e).ok);
    CHECK(e.midiTracks().empty());
}

TEST_CASE("a malformed note is skipped without losing the project",
          "[mididoc][json]") {
    const std::string doc = R"({
        "format": "dave.doc/v1",
        "midiTracks": [{
            "id": "miditrack_1", "name": "Keys",
            "clips": [{"id": "mclip_1", "length": 48000,
                       "notes": [[0,100,60,90,0], [1,2], "nonsense", [48000,100,62,90,0]]}]
        }]
    })";
    document::Edit e;
    auto r = document::deserializeEdit(doc, e);
    REQUIRE(r.ok);
    REQUIRE(e.midiTracks().size() == 1);
    CHECK(e.midiTracks()[0].clips[0].notes.size() == 2);
}

// ─── Undo / redo ────────────────────────────────────────────────────────────

TEST_CASE("adding a MIDI track undoes and redoes", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::AddMidiTrackCommand>("Keys"));
    REQUIRE(e.midiTracks().size() == 1);
    const std::string after = fingerprint(e);

    undo.undo();
    CHECK(fingerprint(e) == before);
    undo.redo();
    CHECK(fingerprint(e) == after);
}

TEST_CASE("removing a MIDI track restores it in place", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    e.addMidiTrack("First");
    const std::string second = e.addMidiTrack("Second");
    e.addMidiTrack("Third");
    e.setMidiInstrument(second, instrumentSlot());
    e.addMidiClip(second, clipWithNotes(0, 48000));
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::RemoveMidiTrackCommand>(second));
    REQUIRE(e.midiTracks().size() == 2);

    undo.undo();
    // Position matters: the track list order is the row order on screen, so
    // restoring "Second" after "Third" would be a visible regression.
    REQUIRE(e.midiTracks().size() == 3);
    CHECK(e.midiTracks()[1].name == "Second");
    CHECK(e.midiTracks()[1].clips.size() == 1);
    CHECK(fingerprint(e) == before);
}

TEST_CASE("importing a MIDI file undoes as one action", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string before = fingerprint(e);

    std::vector<document::MidiTrack> parsed;
    for (int i = 0; i < 3; ++i) {
        document::MidiTrack mt;
        mt.name = "Part " + std::to_string(i + 1);
        mt.clips.push_back(clipWithNotes(0, 48000));
        parsed.push_back(std::move(mt));
    }
    undo.execute(std::make_unique<editing::ImportMidiFileCommand>(parsed, "song.mid"));

    REQUIRE(e.midiTracks().size() == 3);
    CHECK(e.midiTracks()[2].name == "Part 3");
    CHECK(e.midiTracks()[0].clips.size() == 1);
    CHECK(e.midiTracks()[0].clips[0].notes.size() == 3);
    // One import is one undo step, however many tracks the file held.
    CHECK(undo.undoDepth() == 1);

    const std::string after = fingerprint(e);
    undo.undo();
    CHECK(fingerprint(e) == before);
    undo.redo();
    // Ids included: a redo that re-mints them looks identical on screen while
    // orphaning the selection and every id-keyed engine map.
    CHECK(fingerprint(e) == after);
}

TEST_CASE("moving a MIDI clip within a track undoes", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(0, 48000));
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::MoveMidiClipCommand>(t, c, 96000));
    CHECK(e.midiClip(t, c)->timelineStart == 96000);

    undo.undo();
    CHECK(e.midiClip(t, c)->timelineStart == 0);
    CHECK(fingerprint(e) == before);
    undo.redo();
    CHECK(e.midiClip(t, c)->timelineStart == 96000);
}

TEST_CASE("moving a MIDI clip to another track keeps its id and notes",
          "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string a = e.addMidiTrack("A");
    const std::string b = e.addMidiTrack("B");
    const std::string c = e.addMidiClip(a, clipWithNotes(0, 48000));
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::MoveMidiClipCommand>(a, c, 24000, b));
    CHECK(e.midiTrack(a)->clips.empty());
    REQUIRE(e.midiClip(b, c) != nullptr);
    CHECK(e.midiClip(b, c)->timelineStart == 24000);
    CHECK(e.midiClip(b, c)->notes.size() == 3);

    undo.undo();
    REQUIRE(e.midiClip(a, c) != nullptr);
    CHECK(e.midiClip(a, c)->timelineStart == 0);
    CHECK(e.midiTrack(b)->clips.empty());
    CHECK(fingerprint(e) == before);

    undo.redo();
    REQUIRE(e.midiClip(b, c) != nullptr);
    CHECK(e.midiClip(b, c)->timelineStart == 24000);
}

TEST_CASE("trimming a MIDI clip restores all three positions on undo",
          "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(48000, 48000));

    // Head trim: start moves right, sourceOffset moves with it, length shrinks.
    undo.execute(std::make_unique<editing::TrimMidiClipCommand>(
        t, c, 60000, 12000, 36000));
    CHECK(e.midiClip(t, c)->timelineStart == 60000);
    CHECK(e.midiClip(t, c)->sourceOffset == 12000);
    CHECK(e.midiClip(t, c)->length == 36000);

    undo.undo();
    // Restoring only the start would leave the clip's contents shifted by the
    // trim amount — the reason all three are snapshotted together.
    CHECK(e.midiClip(t, c)->timelineStart == 48000);
    CHECK(e.midiClip(t, c)->sourceOffset == 0);
    CHECK(e.midiClip(t, c)->length == 48000);
}

TEST_CASE("removing a MIDI clip restores its notes and id", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(12000, 48000));
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::RemoveMidiClipCommand>(t, c));
    CHECK(e.midiTrack(t)->clips.empty());

    undo.undo();
    REQUIRE(e.midiClip(t, c) != nullptr);
    CHECK(e.midiClip(t, c)->timelineStart == 12000);
    CHECK(e.midiClip(t, c)->notes.size() == 3);
    CHECK(fingerprint(e) == before);
}

TEST_CASE("swapping an instrument restores the previous one with its state",
          "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
        t, instrumentSlot()));
    const std::string firstId = e.midiTrack(t)->instrument.id;

    document::PluginSlot other;
    other.name = "Dexed";
    other.uidString = "11111111222222223333333344444444";
    undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(t, other));
    CHECK(e.midiTrack(t)->instrument.name == "Dexed");

    undo.undo();
    CHECK(e.midiTrack(t)->instrument.name == "Surge XT");
    CHECK(e.midiTrack(t)->instrument.id == firstId);
    // The saved plugin state has to come back too; losing it on undo would
    // silently reset a synth the user had dialled in.
    CHECK(e.midiTrack(t)->instrument.stateBase64 == "AAECAwQ=");

    undo.redo();
    CHECK(e.midiTrack(t)->instrument.name == "Dexed");
    // Redo must reuse the id minted the first time, or the GraphBuilder cache
    // and any open editor window stop resolving.
    const std::string redoneId = e.midiTrack(t)->instrument.id;
    undo.undo();
    undo.redo();
    CHECK(e.midiTrack(t)->instrument.id == redoneId);
}

TEST_CASE("undoing the first instrument leaves the track with none",
          "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
        t, instrumentSlot()));
    undo.undo();
    CHECK(e.midiTrack(t)->instrument.uidString.empty());
    CHECK(e.midiTrack(t)->instrument.id.empty());
}

TEST_CASE("MIDI effect plugins add and remove with undo", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    document::PluginSlot fx;
    fx.name = "Pro-Q";
    fx.uidString = "FEDCBA9876543210FEDCBA9876543210";

    auto add = std::make_unique<editing::AddMidiPluginCommand>(t, fx);
    auto* addPtr = add.get();
    undo.execute(std::move(add));
    const std::string slotId = addPtr->slotId();
    REQUIRE(e.midiTrack(t)->plugins.size() == 1);

    undo.execute(std::make_unique<editing::RemoveMidiPluginCommand>(t, slotId));
    CHECK(e.midiTrack(t)->plugins.empty());

    undo.undo();
    REQUIRE(e.midiTrack(t)->plugins.size() == 1);
    CHECK(e.midiTrack(t)->plugins[0].id == slotId);
    CHECK(e.midiTrack(t)->plugins[0].name == "Pro-Q");

    undo.undo();
    CHECK(e.midiTrack(t)->plugins.empty());
}

TEST_CASE("splitting a MIDI clip covers the same span in two", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(0, 48000));

    undo.execute(std::make_unique<editing::SplitMidiClipCommand>(t, c, 12000));
    REQUIRE(e.midiTrack(t)->clips.size() == 2);
    const auto& left = e.midiTrack(t)->clips[0];
    const auto& right = e.midiTrack(t)->clips[1];
    CHECK(left.timelineStart == 0);
    CHECK(left.length == 12000);
    CHECK(right.timelineStart == 12000);
    CHECK(right.sourceOffset == 12000);
    CHECK(right.length == 36000);
    // The two halves together still cover exactly the original span.
    CHECK(left.length + right.length == 48000);
}

TEST_CASE("undoing a MIDI split restores the original length exactly",
          "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(0, 48000));
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::SplitMidiClipCommand>(t, c, 12000));
    undo.undo();
    REQUIRE(e.midiTrack(t)->clips.size() == 1);
    CHECK(e.midiClip(t, c)->length == 48000);
    CHECK(fingerprint(e) == before);

    // And it stays put however many times the split is replayed.
    for (int i = 0; i < 3; ++i) { undo.redo(); undo.undo(); }
    CHECK(e.midiClip(t, c)->length == 48000);
    CHECK(fingerprint(e) == before);
}

// The audio SplitClipCommand has no such check: it computes both halves by
// subtraction, so a cut past the clip end gives the right half a negative
// length and stretches the left half beyond the original. Right-clicking a
// clip while the playhead sits elsewhere is enough to reach it.
TEST_CASE("a split outside the clip's bounds does nothing", "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(48000, 48000));

    // Before the clip, at its start, at its end, and past it: all would leave a
    // zero-length half behind.
    for (int64_t at : {int64_t(0), int64_t(48000), int64_t(96000), int64_t(150000)}) {
        undo.execute(std::make_unique<editing::SplitMidiClipCommand>(t, c, at));
        CHECK(e.midiTrack(t)->clips.size() == 1);
        CHECK(e.midiClip(t, c)->length == 48000);
    }
}

TEST_CASE("duplicating a MIDI clip places the copy after the original",
          "[mididoc][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    const std::string t = e.addMidiTrack("Keys");
    const std::string c = e.addMidiClip(t, clipWithNotes(24000, 48000));
    const std::string before = fingerprint(e);

    undo.execute(std::make_unique<editing::DuplicateMidiClipCommand>(t, c));
    REQUIRE(e.midiTrack(t)->clips.size() == 2);
    const auto& copy = e.midiTrack(t)->clips[1];
    CHECK(copy.timelineStart == 72000);   // butts up against the original
    CHECK(copy.length == 48000);
    CHECK(copy.notes.size() == 3);
    CHECK(copy.id != c);

    undo.undo();
    CHECK(fingerprint(e) == before);
}

TEST_CASE("a full edit session round-trips through JSON unchanged",
          "[mididoc][json][undo]") {
    document::Edit e;
    editing::UndoStack undo{e};
    undo.execute(std::make_unique<editing::AddMidiTrackCommand>("Keys"));
    const std::string t = e.midiTracks()[0].id;
    undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
        t, instrumentSlot()));
    undo.execute(std::make_unique<editing::AddMidiClipCommand>(
        t, clipWithNotes(0, 48000)));
    const std::string c = e.midiTracks()[0].clips[0].id;
    undo.execute(std::make_unique<editing::MoveMidiClipCommand>(t, c, 96000));

    document::Edit loaded;
    REQUIRE(document::deserializeEdit(document::serializeEdit(e), loaded).ok);
    CHECK(fingerprint(loaded) == fingerprint(e));
}
