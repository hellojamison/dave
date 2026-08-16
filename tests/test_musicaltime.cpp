// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bars, beats and time signatures.
//
// Everything in the document is stored in samples, so this is the only place
// that knows what a bar is. A meter map that reads correctly at bar 1 and
// drifts by bar 40 is the classic failure here, so the round-trip cases walk
// past several changes rather than checking one.
#include "document/Edit.h"
#include "document/MusicalTime.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "gui/Timeline.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using Catch::Approx;
using dave::document::BarsBeats;
using dave::document::TimeSignature;
using dave::document::barsBeatsAtSample;
using dave::document::kTicksPerBeat;
using dave::document::normalizeMeterMap;
using dave::document::quartersPerBar;
using dave::document::quartersPerBeat;
using dave::document::sampleAtBar;
using dave::document::sampleAtBarsBeats;
using dave::document::samplesPerBarAtBar;
using dave::document::samplesPerBeatAtBar;
using dave::document::signatureAtBar;

namespace {
constexpr double kRate = 48000.0;
constexpr double kBpm = 120.0;   // 24,000 samples per quarter
}

// ─── The map itself ────────────────────────────────────────────────────────

TEST_CASE("a meter map always starts at bar one", "[musicaltime]") {
    // Every reader walks the map from the top, so a map whose first change is
    // at bar 9 would leave bars 1-8 undefined.
    const auto empty = normalizeMeterMap({});
    REQUIRE(empty.size() == 1);
    CHECK(empty[0] == TimeSignature{1, 4, 4});

    const auto late = normalizeMeterMap({{9, 3, 4}});
    REQUIRE(late.size() == 2);
    CHECK(late[0] == TimeSignature{1, 4, 4});
    CHECK(late[1] == TimeSignature{9, 3, 4});
}

TEST_CASE("a meter map is sorted and one per bar", "[musicaltime]") {
    const auto map = normalizeMeterMap({{5, 3, 4}, {1, 4, 4}, {5, 7, 8}});
    REQUIRE(map.size() == 2);
    CHECK(map[0].bar == 1);
    CHECK(map[1].bar == 5);
    // Two changes at one bar would make the walk count that bar twice; the
    // later edit wins.
    CHECK(map[1].numerator == 7);
    CHECK(map[1].denominator == 8);
}

TEST_CASE("a nonsense signature is repaired rather than trusted",
          "[musicaltime]") {
    // The map comes from a project file and from a UI that can be mid-edit.
    const auto map = normalizeMeterMap({{1, 4, 4}, {2, 0, 5}, {-3, 4, 4}});
    for (const auto& signature : map) {
        CHECK(signature.bar >= 1);
        CHECK(signature.numerator >= 1);
        // A denominator that is not a power of two has no note value at all.
        const int d = signature.denominator;
        CHECK((d & (d - 1)) == 0);
    }
}

TEST_CASE("a beat is the note the denominator names", "[musicaltime]") {
    CHECK(quartersPerBeat(TimeSignature{1, 4, 4}) == Approx(1.0));
    CHECK(quartersPerBar(TimeSignature{1, 4, 4}) == Approx(4.0));
    // 6/8: the beat is an eighth, so half a quarter, and the bar is three
    // quarters — not six.
    CHECK(quartersPerBeat(TimeSignature{1, 6, 8}) == Approx(0.5));
    CHECK(quartersPerBar(TimeSignature{1, 6, 8}) == Approx(3.0));
    CHECK(quartersPerBar(TimeSignature{1, 3, 4}) == Approx(3.0));
    CHECK(quartersPerBar(TimeSignature{1, 5, 4}) == Approx(5.0));
    CHECK(quartersPerBar(TimeSignature{1, 7, 16}) == Approx(1.75));
}

TEST_CASE("the signature in force is the last one at or before the bar",
          "[musicaltime]") {
    const auto map = normalizeMeterMap({{1, 4, 4}, {5, 3, 4}, {9, 6, 8}});
    CHECK(signatureAtBar(map, 1).numerator == 4);
    CHECK(signatureAtBar(map, 4).numerator == 4);
    CHECK(signatureAtBar(map, 5).numerator == 3);
    CHECK(signatureAtBar(map, 8).numerator == 3);
    CHECK(signatureAtBar(map, 9).numerator == 6);
    CHECK(signatureAtBar(map, 900).numerator == 6);
}

// ─── Position conversion ───────────────────────────────────────────────────

TEST_CASE("bar one beat one is sample zero", "[musicaltime]") {
    const auto map = normalizeMeterMap({});
    CHECK(sampleAtBar(1, kRate, kBpm, map) == 0);
    const auto position = barsBeatsAtSample(0, kRate, kBpm, map);
    CHECK(position == BarsBeats{1, 1, 0});
}

TEST_CASE("bars and beats read correctly in common time", "[musicaltime]") {
    const auto map = normalizeMeterMap({});
    // 24,000 samples per quarter at 120 bpm; a 4/4 bar is 96,000.
    CHECK(barsBeatsAtSample(24000, kRate, kBpm, map) == BarsBeats{1, 2, 0});
    CHECK(barsBeatsAtSample(96000, kRate, kBpm, map) == BarsBeats{2, 1, 0});
    CHECK(barsBeatsAtSample(96000 * 7, kRate, kBpm, map) == BarsBeats{8, 1, 0});

    // Half a beat in is half the ticks.
    const auto half = barsBeatsAtSample(12000, kRate, kBpm, map);
    CHECK(half.bar == 1);
    CHECK(half.beat == 1);
    CHECK(half.tick == kTicksPerBeat / 2);
}

TEST_CASE("a meter change moves every bar line after it", "[musicaltime]") {
    // 4/4 for four bars, then 3/4. Bar 5 starts after 4 x 96,000 samples;
    // bar 6 is three quarters later, not four.
    const auto map = normalizeMeterMap({{1, 4, 4}, {5, 3, 4}});
    CHECK(sampleAtBar(5, kRate, kBpm, map) == 384000);
    CHECK(sampleAtBar(6, kRate, kBpm, map) == 384000 + 72000);
    CHECK(sampleAtBar(7, kRate, kBpm, map) == 384000 + 144000);

    CHECK(barsBeatsAtSample(384000, kRate, kBpm, map) == BarsBeats{5, 1, 0});
    CHECK(barsBeatsAtSample(384000 + 48000, kRate, kBpm, map) ==
          BarsBeats{5, 3, 0});
    // One beat past the end of a 3/4 bar is the next bar, not beat four.
    CHECK(barsBeatsAtSample(384000 + 72000, kRate, kBpm, map) ==
          BarsBeats{6, 1, 0});
}

TEST_CASE("bar length follows the meter at that bar", "[musicaltime]") {
    const auto map = normalizeMeterMap({{1, 4, 4}, {5, 6, 8}});
    CHECK(samplesPerBarAtBar(1, kRate, kBpm, map) == Approx(96000.0));
    CHECK(samplesPerBeatAtBar(1, kRate, kBpm, map) == Approx(24000.0));
    // 6/8: six eighth-note beats of 12,000, so a 72,000-sample bar.
    CHECK(samplesPerBarAtBar(5, kRate, kBpm, map) == Approx(72000.0));
    CHECK(samplesPerBeatAtBar(5, kRate, kBpm, map) == Approx(12000.0));
}

TEST_CASE("position survives a round trip across several meter changes",
          "[musicaltime]") {
    // The failure this guards is drift: correct at bar 1, a beat out by bar
    // 40 because a segment was counted with the wrong meter.
    const auto map =
        normalizeMeterMap({{1, 4, 4}, {5, 3, 4}, {13, 7, 8}, {21, 6, 8},
                           {33, 4, 4}});
    for (int bar = 1; bar <= 48; ++bar) {
        const int64_t sample = sampleAtBar(bar, kRate, kBpm, map);
        const auto position = barsBeatsAtSample(sample, kRate, kBpm, map);
        CHECK(position.bar == bar);
        CHECK(position.beat == 1);
        CHECK(position.tick == 0);
    }

    // And at beats inside those bars, not only on the bar lines.
    for (int bar : {2, 6, 14, 22, 34}) {
        const auto signature = signatureAtBar(map, bar);
        for (int beat = 1; beat <= signature.numerator; ++beat) {
            const BarsBeats wanted{bar, beat, 0};
            const int64_t sample =
                sampleAtBarsBeats(wanted, kRate, kBpm, map);
            CHECK(barsBeatsAtSample(sample, kRate, kBpm, map) == wanted);
        }
    }
}

TEST_CASE("tempo scales everything and zero is refused", "[musicaltime]") {
    const auto map = normalizeMeterMap({});
    // Twice the tempo, half the samples.
    CHECK(sampleAtBar(2, kRate, 240.0, map) == 48000);
    CHECK(sampleAtBar(2, kRate, 60.0, map) == 192000);

    // A zero or negative tempo is repaired to 120 rather than refused. The
    // map's job is to guarantee a usable tempo everywhere so no reader has to
    // check — and a ruler that collapses every position onto bar 1 is a worse
    // answer than the default one, because it looks like a position.
    CHECK(barsBeatsAtSample(48000, kRate, 0.0, map) == BarsBeats{1, 3, 0});
    CHECK(sampleAtBar(4, kRate, 0.0, map) == sampleAtBar(4, kRate, 120.0, map));

    // A broken sample rate is different: that is not a musical value at all,
    // and there is no sensible default for the device.
    CHECK(barsBeatsAtSample(48000, 0.0, kBpm, map) == BarsBeats{1, 1, 0});
}

TEST_CASE("a negative sample is bar one, not a negative bar",
          "[musicaltime]") {
    const auto map = normalizeMeterMap({});
    CHECK(barsBeatsAtSample(-48000, kRate, kBpm, map) == BarsBeats{1, 1, 0});
}

// ─── The document ──────────────────────────────────────────────────────────

TEST_CASE("a session starts at 120 in common time", "[musicaltime][document]") {
    dave::document::Edit edit;
    CHECK(edit.tempoBpm() == Approx(120.0));
    REQUIRE(edit.meterMap().size() == 1);
    CHECK(edit.meterMap()[0] == TimeSignature{1, 4, 4});
}

TEST_CASE("a nonsense tempo is refused, not clamped",
          "[musicaltime][document]") {
    // Zero would divide by zero in every conversion; clamping to some
    // arbitrary floor would silently retune the session instead.
    dave::document::Edit edit;
    edit.setTempoBpm(0.0);
    CHECK(edit.tempoBpm() == Approx(120.0));
    edit.setTempoBpm(-90.0);
    CHECK(edit.tempoBpm() == Approx(120.0));
    edit.setTempoBpm(140.0);
    CHECK(edit.tempoBpm() == Approx(140.0));
}

TEST_CASE("setting a signature replaces the one at that bar",
          "[musicaltime][document]") {
    dave::document::Edit edit;
    REQUIRE(edit.setTimeSignature(9, 3, 4));
    REQUIRE(edit.meterMap().size() == 2);
    REQUIRE(edit.setTimeSignature(9, 7, 8));
    REQUIRE(edit.meterMap().size() == 2);
    CHECK(edit.meterMap()[1] == TimeSignature{9, 7, 8});
    // Setting the same thing twice is not a change.
    CHECK_FALSE(edit.setTimeSignature(9, 7, 8));
}

TEST_CASE("bar one's signature cannot be removed",
          "[musicaltime][document]") {
    // Removing it would leave every bar before the next change with no meter.
    dave::document::Edit edit;
    REQUIRE(edit.setTimeSignature(5, 3, 4));
    CHECK_FALSE(edit.removeTimeSignature(1));
    CHECK(edit.meterMap().size() == 2);
    CHECK(edit.removeTimeSignature(5));
    CHECK(edit.meterMap().size() == 1);
    CHECK_FALSE(edit.removeTimeSignature(5));
}

TEST_CASE("tempo and meter round-trip through the project file",
          "[musicaltime][document]") {
    dave::document::Edit edit;
    edit.addTrack("Audio");
    edit.setTempoBpm(93.5);
    REQUIRE(edit.setTimeSignature(5, 7, 8));
    REQUIRE(edit.setTimeSignature(13, 6, 8));

    const std::string text = dave::document::serializeEdit(edit);
    dave::document::Edit reloaded;
    const auto result = dave::document::deserializeEdit(text, reloaded);
    REQUIRE(result.ok);
    CHECK(reloaded.tempoBpm() == Approx(93.5));
    REQUIRE(reloaded.meterMap().size() == 3);
    CHECK(reloaded.meterMap()[1] == TimeSignature{5, 7, 8});
    CHECK(reloaded.meterMap()[2] == TimeSignature{13, 6, 8});
}

TEST_CASE("a project written before musical time reopens in 4/4 at 120",
          "[musicaltime][document]") {
    // Which is what the ruler was hardcoded to when that file was saved, so
    // it reopens looking the same rather than at some new default.
    dave::document::Edit edit;
    edit.addTrack("Audio");
    std::string text = dave::document::serializeEdit(edit);
    // Strip the fields a pre-musical-time build would not have written.
    const auto strip = [&](const std::string& key) {
        const auto at = text.find("\"" + key + "\"");
        if (at == std::string::npos) return;
        const auto end = text.find_first_of(",}", at);
        if (end == std::string::npos || text[end] != ',') return;
        text.erase(at, end - at + 1);
    };
    strip("tempoBpm");

    dave::document::Edit reloaded;
    REQUIRE(dave::document::deserializeEdit(text, reloaded).ok);
    CHECK(reloaded.tempoBpm() == Approx(120.0));
    REQUIRE(reloaded.meterMap().size() >= 1);
    CHECK(reloaded.meterMap()[0] == TimeSignature{1, 4, 4});
}

TEST_CASE("undo restores the whole map, not just the one bar",
          "[musicaltime][document]") {
    // A change at bar 1 renumbers the session; the only way back is the exact
    // map that was there before.
    dave::document::Edit edit;
    dave::editing::UndoStack undo{edit};
    REQUIRE(edit.setTimeSignature(5, 3, 4));
    const auto before = edit.meterMap();

    undo.execute(std::make_unique<dave::editing::SetTimeSignatureCommand>(
        1, 7, 8));
    CHECK(edit.meterMap()[0] == TimeSignature{1, 7, 8});
    undo.undo();
    CHECK(edit.meterMap() == before);

    undo.execute(std::make_unique<dave::editing::RemoveTimeSignatureCommand>(5));
    CHECK(edit.meterMap().size() == 1);
    undo.undo();
    CHECK(edit.meterMap() == before);
}

// ─── The tempo map ─────────────────────────────────────────────────────────
//
// Tempo is constant between changes — no ramps — so a change takes effect on
// its beat and holds. The failure this guards is the same as the meter map's:
// correct at bar 1, minutes out by bar 100 because a segment was integrated at
// the wrong tempo.

using dave::document::TempoChange;
using dave::document::bpmAt;
using dave::document::constantTempo;
using dave::document::normalizeTempoMap;

TEST_CASE("a tempo map always starts at the session start", "[musicaltime]") {
    const auto empty = normalizeTempoMap({});
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].bar == 1);
    CHECK(empty[0].beat == 1);

    const auto late = normalizeTempoMap({{9, 1, 90.0}});
    REQUIRE(late.size() == 2);
    CHECK(late[0].bpm == Approx(120.0));
    CHECK(late[1].bar == 9);
}

TEST_CASE("a tempo that would divide by zero is repaired", "[musicaltime]") {
    const auto map = normalizeTempoMap({{1, 1, 0.0}, {5, 1, -30.0}});
    for (const auto& change : map) CHECK(change.bpm > 0.0);
}

TEST_CASE("tempo changes are sorted and one per position", "[musicaltime]") {
    const auto map = normalizeTempoMap(
        {{5, 3, 90.0}, {1, 1, 120.0}, {5, 1, 100.0}, {5, 3, 95.0}});
    REQUIRE(map.size() == 3);
    CHECK(map[0].bar == 1);
    CHECK(map[1].beat == 1);
    CHECK(map[2].beat == 3);
    CHECK(map[2].bpm == Approx(95.0));   // last edit wins
}

TEST_CASE("a tempo change re-times everything after it", "[musicaltime]") {
    // 120 for four bars, then 60. Bar 5 is at the same place as before; bar 6
    // takes twice as long as it used to.
    const auto meter = normalizeMeterMap({});
    const auto tempo = normalizeTempoMap({{1, 1, 120.0}, {5, 1, 60.0}});

    CHECK(sampleAtBar(5, kRate, tempo, meter) == 384000);
    CHECK(sampleAtBar(6, kRate, tempo, meter) == 384000 + 192000);
    CHECK(samplesPerBarAtBar(1, kRate, tempo, meter) == Approx(96000.0));
    CHECK(samplesPerBarAtBar(5, kRate, tempo, meter) == Approx(192000.0));

    // And reading back gives the bar you started from.
    CHECK(barsBeatsAtSample(384000, kRate, tempo, meter) == BarsBeats{5, 1, 0});
    CHECK(barsBeatsAtSample(384000 + 192000, kRate, tempo, meter) ==
          BarsBeats{6, 1, 0});
}

TEST_CASE("a tempo change mid-bar only affects the beats after it",
          "[musicaltime]") {
    // 120 until bar 2 beat 3, then 60. Beats 1 and 2 of bar 2 are 24,000
    // samples; beats 3 and 4 are 48,000.
    const auto meter = normalizeMeterMap({});
    const auto tempo = normalizeTempoMap({{1, 1, 120.0}, {2, 3, 60.0}});

    CHECK(sampleAtBar(2, kRate, tempo, meter) == 96000);
    CHECK(sampleAtBarsBeats(BarsBeats{2, 3, 0}, kRate, tempo, meter) ==
          96000 + 48000);
    // Two beats at half speed close the bar.
    CHECK(sampleAtBar(3, kRate, tempo, meter) == 96000 + 48000 + 96000);
}

TEST_CASE("tempo and meter changes compose", "[musicaltime]") {
    const auto meter = normalizeMeterMap({{1, 4, 4}, {5, 3, 4}});
    const auto tempo = normalizeTempoMap({{1, 1, 120.0}, {5, 1, 60.0}});
    // Bar 5 onwards: 3/4 at 60 bpm, so three beats of 48,000.
    CHECK(sampleAtBar(5, kRate, tempo, meter) == 384000);
    CHECK(samplesPerBeatAtBar(5, kRate, tempo, meter) == Approx(48000.0));
    CHECK(samplesPerBarAtBar(5, kRate, tempo, meter) == Approx(144000.0));

    // A tempo change is anchored musically, so it sits at bar 5 whatever the
    // meter before it did to the sample position of bar 5.
    CHECK(bpmAt(tempo, meter, 4) == Approx(120.0));
    CHECK(bpmAt(tempo, meter, 5) == Approx(60.0));
}

TEST_CASE("position survives a round trip across tempo changes",
          "[musicaltime]") {
    const auto meter = normalizeMeterMap({{1, 4, 4}, {9, 3, 4}, {17, 7, 8}});
    const auto tempo = normalizeTempoMap(
        {{1, 1, 120.0}, {5, 1, 90.0}, {9, 2, 144.0}, {20, 1, 72.5}});
    for (int bar = 1; bar <= 40; ++bar) {
        const int64_t sample = sampleAtBar(bar, kRate, tempo, meter);
        const auto position = barsBeatsAtSample(sample, kRate, tempo, meter);
        CHECK(position.bar == bar);
        CHECK(position.beat == 1);
    }
}

TEST_CASE("a single-tempo call means a one-entry map", "[musicaltime]") {
    // The overloads exist so callers with one tempo do not have to build a
    // map; they have to agree with one that does.
    const auto meter = normalizeMeterMap({{1, 4, 4}, {5, 3, 4}});
    const auto tempo = constantTempo(93.0);
    for (int bar = 1; bar <= 12; ++bar) {
        CHECK(sampleAtBar(bar, kRate, 93.0, meter) ==
              sampleAtBar(bar, kRate, tempo, meter));
    }
}

TEST_CASE("the tempo map round-trips and undoes as a whole",
          "[musicaltime][document]") {
    dave::document::Edit edit;
    edit.addTrack("Audio");
    dave::editing::UndoStack undo{edit};

    REQUIRE(edit.setTempoChange(5, 1, 90.0));
    REQUIRE(edit.setTempoChange(9, 3, 144.0));
    // The bar-1 accessor still answers the session's starting tempo.
    CHECK(edit.tempoBpm() == Approx(120.0));

    const std::string text = dave::document::serializeEdit(edit);
    dave::document::Edit reloaded;
    REQUIRE(dave::document::deserializeEdit(text, reloaded).ok);
    REQUIRE(reloaded.tempoMap().size() == 3);
    CHECK(reloaded.tempoMap()[1] == TempoChange{5, 1, 90.0});
    CHECK(reloaded.tempoMap()[2] == TempoChange{9, 3, 144.0});

    const auto before = edit.tempoMap();
    undo.execute(std::make_unique<dave::editing::SetTempoCommand>(1, 1, 70.0));
    CHECK(edit.tempoBpm() == Approx(70.0));
    undo.undo();
    CHECK(edit.tempoMap() == before);

    // The session's own tempo is not a change and cannot be removed.
    CHECK_FALSE(edit.removeTempoChange(1, 1));
    CHECK(edit.removeTempoChange(5, 1));
}

TEST_CASE("a project from before the tempo map keeps its tempo",
          "[musicaltime][document]") {
    // Its single tempoBpm becomes the bar-1 entry rather than reverting to
    // 120, which would retune the session on open.
    dave::document::Edit edit;
    edit.addTrack("Audio");
    edit.setTempoBpm(97.0);
    std::string text = dave::document::serializeEdit(edit);
    const auto at = text.find("\"tempoMap\"");
    REQUIRE(at != std::string::npos);
    const auto end = text.find(']', at);
    REQUIRE(end != std::string::npos);
    text.erase(at, end - at + 2);   // the array and its trailing comma

    dave::document::Edit reloaded;
    REQUIRE(dave::document::deserializeEdit(text, reloaded).ok);
    CHECK(reloaded.tempoBpm() == Approx(97.0));
    REQUIRE(reloaded.tempoMap().size() == 1);
}

TEST_CASE("bars, beats and ticks are separated by pipes", "[musicaltime]") {
    // A dot reads as a decimal point: "2.3.480" looks like one number where
    // "2|3|480" reads as the three fields it is.
    const auto meter = normalizeMeterMap({});
    const auto tempo = constantTempo(120.0);
    const std::string start = dave::gui::formatTimecode(
        0, dave::gui::TimecodeMode::BarsBeats, kRate, 24.0, 120.0, &meter,
        &tempo);
    CHECK(start == "1|1|000");

    // One bar in at 120 bpm in 4/4.
    const std::string barTwo = dave::gui::formatTimecode(
        96000, dave::gui::TimecodeMode::BarsBeats, kRate, 24.0, 120.0, &meter,
        &tempo);
    CHECK(barTwo == "2|1|000");
}
