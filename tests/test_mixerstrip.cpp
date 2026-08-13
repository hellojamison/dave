// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mixer strip wiring, driven through the real drawMixer.
//
// The insert chain itself is old and already routed by GraphBuilder; what is
// new is the mixer reaching it. These tests press the actual controls and then
// ask the document (or the pending request) what happened, because the failure
// mode worth catching here is a button that draws correctly and does nothing.
#include "ImGuiTestRig.h"

#include "gui/Mixer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dave;
using dave::testing::ImGuiRig;
using Picker = gui::TimelineViewState::PluginPicker;

namespace {

constexpr float kStripWidth = 108.0f;
constexpr float kStripGap = 3.0f;

document::PluginSlot slotNamed(const std::string& name) {
    document::PluginSlot s;
    s.name = name;
    s.uidString = "0123456789ABCDEF0123456789ABCDEF";
    s.path = "/nowhere/" + name + ".vst3";
    return s;
}

// Sweep down one strip's column pressing once per row until `stop` reports the
// control fired. Returns false if nothing in the column responded, which is a
// real failure — it means the control is not where the strip claims it is.
// `xFraction` picks where across the strip to press: 0.5 is the middle, which
// lands between the side-by-side mute and solo buttons, so those need a
// quarter-width probe.
template <typename StopFn>
bool clickDownColumn(ImGuiRig& rig, int stripIndex, const StopFn& stop,
                     float xFraction = 0.5f) {
    const float x =
        12.0f + stripIndex * (kStripWidth + kStripGap) + kStripWidth * xFraction;
    auto body = [&] { gui::drawMixer(rig.edit, rig.undo, rig.view, kStripWidth); };
    for (float y = 20.0f; y < ImGuiRig::kDisplayH - 20.0f; y += 3.0f) {
        rig.clickAt(x, y, body);
        if (stop()) return true;
    }
    return false;
}

template <typename StopFn>
bool optionClickDownColumn(ImGuiRig& rig, int stripIndex, const StopFn& stop) {
    const float x =
        12.0f + stripIndex * (kStripWidth + kStripGap) + kStripWidth * 0.5f;
    auto body = [&] { gui::drawMixer(rig.edit, rig.undo, rig.view, kStripWidth); };
    for (float y = 20.0f; y < ImGuiRig::kDisplayH - 20.0f; y += 3.0f) {
        rig.optionClickAt(x, y, body);
        if (stop()) return true;
    }
    return false;
}

template <typename StopFn>
bool dragUpColumn(ImGuiRig& rig, int stripIndex, const StopFn& stop) {
    const float x =
        12.0f + stripIndex * (kStripWidth + kStripGap) + kStripWidth * 0.5f;
    auto body = [&] { gui::drawMixer(rig.edit, rig.undo, rig.view, kStripWidth); };
    for (float y = 20.0f; y < ImGuiRig::kDisplayH - 40.0f; y += 3.0f) {
        rig.frame(x, y, false, body);
        rig.frame(x, y, true, body);
        rig.frame(x, y - 18.0f, true, body);
        rig.frame(x, y - 18.0f, false, body);
        if (stop()) return true;
    }
    return false;
}

} // namespace

TEST_CASE("the add-insert row asks for the audio effect picker", "[mixerstrip]") {
    ImGuiRig rig;
    const std::string t = rig.edit.addTrack("Drums");

    const bool fired = clickDownColumn(rig, 0, [&] {
        return rig.view.requestPicker != Picker::None;
    });
    REQUIRE(fired);
    CHECK(rig.view.requestPicker == Picker::AudioFx);
    CHECK(rig.view.requestPickerTrackId == t);
}

TEST_CASE("a MIDI strip's instrument row asks for the instrument picker",
          "[mixerstrip]") {
    // The instrument row sits above the inserts, so sweeping downward hits it
    // first. Picking an EQ where an instrument belongs is exactly the mistake
    // the mode filter exists to prevent, so which picker opens matters.
    ImGuiRig rig;
    const std::string t = rig.edit.addMidiTrack("Keys");

    const bool fired = clickDownColumn(rig, 0, [&] {
        return rig.view.requestPicker != Picker::None;
    });
    REQUIRE(fired);
    CHECK(rig.view.requestPicker == Picker::MidiInstrument);
    CHECK(rig.view.requestPickerTrackId == t);
}

TEST_CASE("a MIDI strip with an instrument asks for the MIDI effect picker",
          "[mixerstrip]") {
    ImGuiRig rig;
    const std::string t = rig.edit.addMidiTrack("Keys");
    rig.edit.setMidiInstrument(t, slotNamed("Surge XT"));

    // With the instrument filled, the first empty row down the column is the
    // insert one — which must open the effect list, not the instrument list.
    const bool fired = clickDownColumn(rig, 0, [&] {
        return rig.view.requestPicker != Picker::None;
    });
    REQUIRE(fired);
    CHECK(rig.view.requestPicker == Picker::MidiFx);
    CHECK(rig.view.requestPickerTrackId == t);
}

TEST_CASE("clicking a filled insert asks to open that plugin's editor",
          "[mixerstrip]") {
    ImGuiRig rig;
    const std::string t = rig.edit.addTrack("Vox");
    const std::string slotId = rig.edit.addPlugin(t, slotNamed("Pro-Q"));
    REQUIRE_FALSE(slotId.empty());

    const bool fired = clickDownColumn(rig, 0, [&] {
        return !rig.view.requestPluginEditorSlotId.empty();
    });
    REQUIRE(fired);
    CHECK(rig.view.requestPluginEditorSlotId == slotId);
}

TEST_CASE("inserts keep their chain order in the strip", "[mixerstrip]") {
    // The strip draws the chain top to bottom in signal order. If it ever
    // sorted or reversed them, the mixer would be lying about what the audio
    // actually passes through.
    ImGuiRig rig;
    const std::string t = rig.edit.addTrack("Bus");
    const std::string first = rig.edit.addPlugin(t, slotNamed("Gate"));
    const std::string second = rig.edit.addPlugin(t, slotNamed("Comp"));
    const std::string third = rig.edit.addPlugin(t, slotNamed("EQ"));

    // Sweeping downward should meet them in the order they sit in the chain.
    std::vector<std::string> seen;
    const float x = 12.0f + kStripWidth * 0.5f;
    auto body = [&] { gui::drawMixer(rig.edit, rig.undo, rig.view, kStripWidth); };
    for (float y = 20.0f; y < ImGuiRig::kDisplayH - 20.0f; y += 3.0f) {
        rig.view.requestPluginEditorSlotId.clear();
        rig.clickAt(x, y, body);
        const std::string& hit = rig.view.requestPluginEditorSlotId;
        if (!hit.empty() && (seen.empty() || seen.back() != hit)) {
            seen.push_back(hit);
        }
    }
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == first);
    CHECK(seen[1] == second);
    CHECK(seen[2] == third);
}

TEST_CASE("the mute button on a strip mutes that track", "[mixerstrip]") {
    ImGuiRig rig;
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");

    const bool fired = clickDownColumn(rig, 1, [&] {
        return rig.edit.track(b)->mute;
    }, 0.5f);   // audio rows are R / M / S; mute occupies the middle third
    REQUIRE(fired);
    // Strip 1 is the SECOND track: a strip that edited its neighbour would be
    // invisible in a screenshot and obvious here.
    CHECK(rig.edit.track(b)->mute);
    CHECK_FALSE(rig.edit.track(a)->mute);
}

TEST_CASE("the record button arms only its audio strip",
          "[mixerstrip][record-arm]") {
    ImGuiRig rig;
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");

    const bool fired = clickDownColumn(rig, 1, [&] {
        return rig.edit.track(b)->recordArm;
    }, 0.16f);   // R is the left third of an audio strip's R/M/S row
    REQUIRE(fired);
    CHECK(rig.edit.track(b)->recordArm);
    CHECK_FALSE(rig.edit.track(a)->recordArm);
}

TEST_CASE("MIDI strips preserve their two-button mute and solo layout",
          "[mixerstrip][record-arm]") {
    ImGuiRig rig;
    const std::string midi = rig.edit.addMidiTrack("Keys");

    const bool fired = clickDownColumn(rig, 0, [&] {
        return rig.edit.midiTrack(midi)->mute;
    }, 0.25f);   // still the left half; no audio-only R was inserted
    REQUIRE(fired);
    CHECK(rig.edit.midiTrack(midi)->mute);
    CHECK_FALSE(rig.edit.midiTrack(midi)->solo);
}

TEST_CASE("an empty session draws a mixer without crashing", "[mixerstrip]") {
    ImGuiRig rig;
    rig.clickAt(200.0f, 200.0f,
                [&] { gui::drawMixer(rig.edit, rig.undo, rig.view, kStripWidth); });
    CHECK(rig.edit.tracks().empty());
    CHECK(rig.view.requestPicker == Picker::None);
}

// ─── Pan readout ────────────────────────────────────────────────────────────

TEST_CASE("pan reads as a side and a magnitude, not a signed fraction",
          "[pan]") {
    using dave::gui::theme::formatPan;

    // Centre is a side of its own — "L0" and "R0" are the same position and
    // showing either implies an offset that is not there.
    CHECK(formatPan(0.0) == "C");

    CHECK(formatPan(-0.5) == "L50");
    CHECK(formatPan(0.5) == "R50");
    CHECK(formatPan(-1.0) == "L100");
    CHECK(formatPan(1.0) == "R100");

    // Rounds to whole units rather than showing a fraction of one.
    CHECK(formatPan(-0.254) == "L25");
    CHECK(formatPan(0.256) == "R26");

    // Values a hair off centre round to it rather than reading as hard left.
    CHECK(formatPan(0.001) == "C");
    CHECK(formatPan(-0.001) == "C");

    // Symmetric either side, at every step.
    for (int i = 0; i <= 100; ++i) {
        const double v = i / 100.0;
        const std::string l = formatPan(-v);
        const std::string r = formatPan(v);
        if (i == 0) {
            REQUIRE(l == "C");
            REQUIRE(r == "C");
        } else {
            REQUIRE(l == "L" + std::to_string(i));
            REQUIRE(r == "R" + std::to_string(i));
        }
    }
}

TEST_CASE("dragging the mixer pan knob changes only that track's pan",
          "[mixerstrip][pan]") {
    ImGuiRig rig;
    const std::string a = rig.edit.addTrack("A");
    const std::string b = rig.edit.addTrack("B");

    const bool fired = dragUpColumn(rig, 1, [&] {
        return rig.edit.track(b)->pan > 0.0;
    });
    REQUIRE(fired);
    CHECK(rig.edit.track(b)->pan > 0.0);
    CHECK(rig.edit.track(a)->pan == 0.0);
}

TEST_CASE("Option-clicking mixer pan resets it to center",
          "[mixerstrip][pan][reset]") {
    ImGuiRig rig;
    const std::string track = rig.edit.addTrack("A");
    rig.edit.track(track)->pan = 0.73;

    REQUIRE(optionClickDownColumn(rig, 0, [&] {
        return rig.edit.track(track)->pan == 0.0;
    }));
    CHECK(rig.edit.track(track)->pan == 0.0);
}

TEST_CASE("Option-clicking mixer volume resets it to zero dB",
          "[mixerstrip][gain][reset]") {
    ImGuiRig rig;
    const std::string track = rig.edit.addTrack("A");
    rig.edit.track(track)->gain = 0.25;

    REQUIRE(optionClickDownColumn(rig, 0, [&] {
        return rig.edit.track(track)->gain == 1.0;
    }));
    CHECK(rig.edit.track(track)->gain == 1.0);
}
