// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "gui/Timeline.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "engine/GraphBuilder.h"
#include "engine/nodes/GainNode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <memory>

namespace {
// Main is a row in the one track list now, so a test asking "how many tracks
// did I make?" has to say so. Counting user rows keeps the intent visible
// rather than burying a +1 in every expectation.
inline size_t userTracks(const dave::document::Edit& e) {
    size_t n = 0;
    for (const auto& t : e.tracks()) if (!t.isMain) ++n;
    return n;
}
} // namespace

using Catch::Approx;

TEST_CASE("volume automation points are ordered, bounded, and undoable",
          "[automation][document]") {
    dave::document::Edit edit;
    const std::string track = edit.addTrack("Dialog");
    dave::editing::UndoStack undo(edit);

    undo.execute(std::make_unique<
        dave::editing::AddVolumeAutomationPointCommand>(track, 48000, -12.0));
    undo.execute(std::make_unique<
        dave::editing::AddVolumeAutomationPointCommand>(track, 0, 99.0));

    const auto* points = edit.volumeAutomation(track);
    REQUIRE(points != nullptr);
    REQUIRE(points->size() == 2);
    CHECK((*points)[0].sample == 0);
    CHECK((*points)[0].db == dave::document::kMaxVolumeAutomationDb);
    CHECK((*points)[1].sample == 48000);
    const std::string secondId = (*points)[1].id;

    undo.undo();
    REQUIRE(edit.volumeAutomation(track)->size() == 1);
    undo.redo();
    REQUIRE(edit.volumeAutomation(track)->size() == 2);
    CHECK((*edit.volumeAutomation(track))[1].id == secondId);

    CHECK(edit.addVolumeAutomationPoint(track, 48000, 0.0).empty());
    CHECK(dave::document::volumeAutomationDbAt(
              *edit.volumeAutomation(track), 24000) == Approx(-3.0));

    auto moved = (*edit.volumeAutomation(track))[1];
    moved.sample = 72000;
    moved.db = -18.0;
    undo.execute(std::make_unique<
        dave::editing::MoveVolumeAutomationPointCommand>(track, moved));
    CHECK((*edit.volumeAutomation(track))[1].sample == 72000);
    CHECK((*edit.volumeAutomation(track))[1].db == Approx(-18.0));
    undo.undo();
    CHECK((*edit.volumeAutomation(track))[1].sample == 48000);
    undo.redo();
    CHECK((*edit.volumeAutomation(track))[1].id == secondId);
    CHECK((*edit.volumeAutomation(track))[1].sample == 72000);

    const std::string firstId = (*edit.volumeAutomation(track))[0].id;
    undo.execute(std::make_unique<
        dave::editing::RemoveVolumeAutomationPointCommand>(track, firstId));
    REQUIRE(edit.volumeAutomation(track)->size() == 1);
    undo.undo();
    REQUIRE(edit.volumeAutomation(track)->size() == 2);
    CHECK((*edit.volumeAutomation(track))[0].id == firstId);
}

TEST_CASE("pan automation points are ordered, bounded, and undoable",
          "[automation][document]") {
    dave::document::Edit edit;
    const std::string track = edit.addTrack("Dialog");
    dave::editing::UndoStack undo(edit);

    undo.execute(std::make_unique<
        dave::editing::AddPanAutomationPointCommand>(track, 48000, 0.5));
    undo.execute(std::make_unique<
        dave::editing::AddPanAutomationPointCommand>(track, 0, -9.0));
    const auto* points = edit.panAutomation(track);
    REQUIRE(points != nullptr);
    REQUIRE(points->size() == 2);
    CHECK((*points)[0].sample == 0);
    CHECK((*points)[0].pan == -1.0);
    CHECK((*points)[1].pan == Approx(0.5));
    CHECK(dave::document::panAutomationAt(*points, 24000) == Approx(-0.25));

    const std::string movedId = (*points)[1].id;
    auto moved = (*points)[1];
    moved.sample = 72000;
    moved.pan = 0.75;
    undo.execute(std::make_unique<
        dave::editing::MovePanAutomationPointCommand>(track, moved));
    CHECK(edit.panAutomation(track)->back().sample == 72000);
    CHECK(edit.panAutomation(track)->back().pan == Approx(0.75));
    undo.undo();
    CHECK(edit.panAutomation(track)->back().sample == 48000);
    undo.redo();
    CHECK(edit.panAutomation(track)->back().id == movedId);

    undo.execute(std::make_unique<
        dave::editing::RemovePanAutomationPointCommand>(track, movedId));
    REQUIRE(edit.panAutomation(track)->size() == 1);
    undo.undo();
    REQUIRE(edit.panAutomation(track)->size() == 2);
    CHECK(edit.panAutomation(track)->back().id == movedId);
}

TEST_CASE("draw commands replace an envelope in one stable undo step",
          "[automation][document]") {
    SECTION("volume") {
        dave::document::Edit edit;
        const std::string track = edit.addTrack("Dialog");
        edit.addVolumeAutomationPoint(track, 0, -12.0);
        edit.addVolumeAutomationPoint(track, 48000, 0.0);
        const auto before = *edit.volumeAutomation(track);
        auto after = before;
        after.insert(after.begin() + 1, {{}, 24000, -6.0});
        int notifications = 0;
        edit.setChangeListener([&] { ++notifications; });
        dave::editing::UndoStack undo(edit);

        undo.execute(std::make_unique<
            dave::editing::ReplaceVolumeAutomationCommand>(track, after));
        REQUIRE(undo.undoDepth() == 1);
        CHECK(notifications == 1);
        REQUIRE(edit.volumeAutomation(track)->size() == 3);
        const std::string drawnId = (*edit.volumeAutomation(track))[1].id;
        CHECK_FALSE(drawnId.empty());

        undo.undo();
        CHECK(notifications == 2);
        CHECK(*edit.volumeAutomation(track) == before);
        undo.redo();
        CHECK(notifications == 3);
        CHECK((*edit.volumeAutomation(track))[1].id == drawnId);
    }

    SECTION("pan") {
        dave::document::Edit edit;
        const std::string bus = edit.addBus("Stem");
        edit.addPanAutomationPoint(bus, 0, -1.0);
        edit.addPanAutomationPoint(bus, 48000, 1.0);
        const auto before = *edit.panAutomation(bus);
        auto after = before;
        after.insert(after.begin() + 1, {{}, 24000, 0.0});
        int notifications = 0;
        edit.setChangeListener([&] { ++notifications; });
        dave::editing::UndoStack undo(edit);

        undo.execute(std::make_unique<
            dave::editing::ReplacePanAutomationCommand>(bus, after));
        REQUIRE(undo.undoDepth() == 1);
        CHECK(notifications == 1);
        REQUIRE(edit.panAutomation(bus)->size() == 3);
        const std::string drawnId = (*edit.panAutomation(bus))[1].id;
        CHECK_FALSE(drawnId.empty());

        undo.undo();
        CHECK(notifications == 2);
        CHECK(*edit.panAutomation(bus) == before);
        undo.redo();
        CHECK(notifications == 3);
        CHECK((*edit.panAutomation(bus))[1].id == drawnId);
    }
}

TEST_CASE("volume and pan automation round-trip on every channel type",
          "[automation][document][persistence]") {
    dave::document::Edit edit;
    const std::string audio = edit.addTrack("Audio");
    const std::string midi = edit.addMidiTrack("Instrument");
    const std::string bus = edit.addBus("Stem");
    REQUIRE_FALSE(edit.addVolumeAutomationPoint(audio, 10, -3.0).empty());
    REQUIRE_FALSE(edit.addVolumeAutomationPoint(midi, 20, -6.0).empty());
    REQUIRE_FALSE(edit.addVolumeAutomationPoint(bus, 30, 3.0).empty());
    REQUIRE_FALSE(edit.addVolumeAutomationPoint(
        dave::document::kMainBusId, 40, -1.0).empty());
    REQUIRE_FALSE(edit.addPanAutomationPoint(audio, 10, -0.75).empty());
    REQUIRE_FALSE(edit.addPanAutomationPoint(midi, 20, 0.25).empty());
    REQUIRE_FALSE(edit.addPanAutomationPoint(bus, 30, 1.0).empty());
    REQUIRE_FALSE(edit.addPanAutomationPoint(
        dave::document::kMainBusId, 40, -0.5).empty());

    const std::string json = dave::document::serializeEdit(edit);
    CHECK(json.find("dave.doc/v4") != std::string::npos);
    dave::document::Edit loaded;
    REQUIRE(dave::document::deserializeEdit(json, loaded).ok);
    REQUIRE(loaded.track(audio)->volumeAutomation.size() == 1);
    REQUIRE(loaded.track(midi)->volumeAutomation.size() == 1);
    REQUIRE(loaded.track(bus)->volumeAutomation.size() == 1);
    REQUIRE(loaded.mainBus()->volumeAutomation.size() == 1);
    CHECK(loaded.track(audio)->volumeAutomation[0].db == Approx(-3.0));
    CHECK(loaded.track(midi)->volumeAutomation[0].db == Approx(-6.0));
    CHECK(loaded.track(bus)->volumeAutomation[0].db == Approx(3.0));
    REQUIRE(loaded.track(audio)->panAutomation.size() == 1);
    REQUIRE(loaded.track(midi)->panAutomation.size() == 1);
    REQUIRE(loaded.track(bus)->panAutomation.size() == 1);
    REQUIRE(loaded.mainBus()->panAutomation.size() == 1);
    CHECK(loaded.track(audio)->panAutomation[0].pan == Approx(-0.75));
    CHECK(loaded.track(midi)->panAutomation[0].pan == Approx(0.25));
    CHECK(loaded.track(bus)->panAutomation[0].pan == Approx(1.0));
}

TEST_CASE("GainNode evaluates volume automation at every sample",
          "[automation][graph]") {
    dave::engine::GainNode gain;
    gain.setGain(1.0);
    gain.setVolumeAutomation({{0, -6.0}, {4, 0.0}});
    gain.prepare(48000.0, 8);

    std::array<float, 8> input{};
    std::array<float, 8> output{};
    input.fill(1.0f);
    float* inputPointer = input.data();
    float* outputPointer = output.data();
    dave::engine::AudioBus inputBus{&inputPointer, 1, 8};
    dave::engine::TimeInfo time;
    time.samplePos = 0;
    dave::engine::NodeProcessContext context;
    context.numSamples = 8;
    context.time = &time;
    context.inputs = &inputBus;
    context.numInputs = 1;
    context.output = dave::engine::AudioBus{&outputPointer, 1, 8};
    gain.process(context);

    for (int sample = 0; sample < 4; ++sample) {
        const double db = -6.0 + 1.5 * sample;
        CHECK(output[static_cast<size_t>(sample)] ==
              Approx(std::pow(10.0, db / 20.0)).margin(1.0e-6));
    }
    CHECK(output[4] == Approx(1.0f).margin(1.0e-6));
    CHECK(output[7] == Approx(1.0f).margin(1.0e-6));
}

TEST_CASE("GainNode evaluates pan automation at every sample",
          "[automation][graph]") {
    dave::engine::GainNode gain;
    gain.setGain(1.0);
    // A written envelope is absolute; the static knob is only used when the
    // pan lane has no points.
    gain.setPan(0.5);
    gain.setPanAutomation({{0, -1.0}, {4, 1.0}});
    gain.prepare(48000.0, 8);

    std::array<float, 8> leftInput{};
    std::array<float, 8> rightInput{};
    std::array<float, 8> leftOutput{};
    std::array<float, 8> rightOutput{};
    leftInput.fill(1.0f);
    rightInput.fill(1.0f);
    std::array<float*, 2> inputPointers{leftInput.data(), rightInput.data()};
    std::array<float*, 2> outputPointers{leftOutput.data(), rightOutput.data()};
    dave::engine::AudioBus inputBus{inputPointers.data(), 2, 8};
    dave::engine::TimeInfo time;
    time.samplePos = 0;
    dave::engine::NodeProcessContext context;
    context.numSamples = 8;
    context.time = &time;
    context.inputs = &inputBus;
    context.numInputs = 1;
    context.output = dave::engine::AudioBus{outputPointers.data(), 2, 8};
    gain.process(context);

    CHECK(leftOutput[0] == Approx(1.0f).margin(1.0e-6));
    CHECK(rightOutput[0] == Approx(0.0f).margin(1.0e-6));
    CHECK(leftOutput[2] == Approx(std::sqrt(0.5)).margin(1.0e-6));
    CHECK(rightOutput[2] == Approx(std::sqrt(0.5)).margin(1.0e-6));
    CHECK(leftOutput[4] == Approx(0.0f).margin(1.0e-6));
    CHECK(rightOutput[4] == Approx(1.0f).margin(1.0e-6));
    CHECK(rightOutput[7] == Approx(1.0f).margin(1.0e-6));
}

TEST_CASE("GraphBuilder publishes automation for every channel type",
          "[automation][graph]") {
    dave::document::Edit edit;
    const std::string audio = edit.addTrack("Audio");
    const std::string midi = edit.addMidiTrack("Instrument");
    const std::string bus = edit.addBus("Stem");
    edit.addVolumeAutomationPoint(audio, 1, -1.0);
    edit.addVolumeAutomationPoint(midi, 2, -2.0);
    edit.addVolumeAutomationPoint(bus, 3, -3.0);
    edit.addVolumeAutomationPoint(dave::document::kMainBusId, 4, -4.0);
    edit.addPanAutomationPoint(audio, 1, -1.0);
    edit.addPanAutomationPoint(midi, 2, -0.5);
    edit.addPanAutomationPoint(bus, 3, 0.5);
    edit.addPanAutomationPoint(dave::document::kMainBusId, 4, 1.0);

    dave::engine::GraphBuilder builder;
    const auto graph = builder.build(edit, 48000.0, 2);
    REQUIRE(graph != nullptr);
    for (const auto& id : {audio, midi, bus,
                           std::string(dave::document::kMainBusId)}) {
        const auto found = builder.trackGains().find(id);
        REQUIRE(found != builder.trackGains().end());
        REQUIRE(found->second->volumeAutomation().size() == 1);
        REQUIRE(found->second->panAutomation().size() == 1);
    }
    CHECK(builder.trackGains().at(audio)->volumeAutomation()[0].db ==
          Approx(-1.0));
    CHECK(builder.trackGains().at(midi)->volumeAutomation()[0].db ==
          Approx(-2.0));
    CHECK(builder.trackGains().at(bus)->volumeAutomation()[0].db ==
          Approx(-3.0));
    CHECK(builder.trackGains()
              .at(dave::document::kMainBusId)
              ->volumeAutomation()[0]
              .db == Approx(-4.0));
    CHECK(builder.trackGains().at(audio)->panAutomation()[0].pan ==
          Approx(-1.0));
    CHECK(builder.trackGains().at(midi)->panAutomation()[0].pan ==
          Approx(-0.5));
    CHECK(builder.trackGains().at(bus)->panAutomation()[0].pan ==
          Approx(0.5));
    CHECK(builder.trackGains()
              .at(dave::document::kMainBusId)
              ->panAutomation()[0]
              .pan == Approx(1.0));
}

// ─── Deleting a range of automation ────────────────────────────────────────

TEST_CASE("delete clears the automation points inside the selection",
          "[automation]") {
    dave::document::Edit edit;
    dave::editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    for (int i = 0; i < 5; ++i) {
        REQUIRE_FALSE(edit.addVolumeAutomationPoint(
            t, i * 48000, -6.0 + i).empty());
    }

    dave::gui::TimelineViewState view;
    view.selectedTrackIndex = 0;
    REQUIRE(edit.tracks()[0].id == t);
    view.expandedTracks.insert(t);
    view.hasSelection = true;
    view.selectionStart = 48000;
    view.selectionEnd = 144000;   // covers points 1, 2 and 3

    REQUIRE(dave::gui::deleteAutomationInSelection(edit, undo, view));
    const auto* points = edit.volumeAutomation(t);
    REQUIRE(points != nullptr);
    REQUIRE(points->size() == 2);
    CHECK((*points)[0].sample == 0);
    CHECK((*points)[1].sample == 192000);

    // One undo entry for the whole range, not one per point.
    undo.undo();
    CHECK(edit.volumeAutomation(t)->size() == 5);
}

TEST_CASE("a backwards selection still names a range", "[automation]") {
    dave::document::Edit edit;
    dave::editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    for (int i = 0; i < 3; ++i) {
        edit.addVolumeAutomationPoint(t, i * 48000, 0.0);
    }

    dave::gui::TimelineViewState view;
    view.selectedTrackIndex = 0;
    view.expandedTracks.insert(t);
    view.hasSelection = true;
    // Dragged right to left.
    view.selectionStart = 96000;
    view.selectionEnd = 48000;

    REQUIRE(dave::gui::deleteAutomationInSelection(edit, undo, view));
    REQUIRE(edit.volumeAutomation(t)->size() == 1);
    CHECK(edit.volumeAutomation(t)->front().sample == 0);
}

TEST_CASE("delete acts on the lane the track is showing", "[automation]") {
    dave::document::Edit edit;
    dave::editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    edit.addVolumeAutomationPoint(t, 48000, 0.0);
    edit.addPanAutomationPoint(t, 48000, 0.5);

    dave::gui::TimelineViewState view;
    view.selectedTrackIndex = 0;
    view.expandedTracks.insert(t);
    view.hasSelection = true;
    view.selectionStart = 0;
    view.selectionEnd = 96000;
    view.automationParameters[t] = dave::gui::AutomationParameter::Pan;

    REQUIRE(dave::gui::deleteAutomationInSelection(edit, undo, view));
    // Pan cleared, volume untouched — the other lane is not visible, and an
    // edit to something you cannot see has no visible cause.
    CHECK(edit.panAutomation(t)->empty());
    CHECK(edit.volumeAutomation(t)->size() == 1);
}

TEST_CASE("delete does nothing without a selection or an open lane",
          "[automation]") {
    dave::document::Edit edit;
    dave::editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    edit.addVolumeAutomationPoint(t, 48000, 0.0);

    dave::gui::TimelineViewState view;
    view.selectedTrackIndex = 0;
    view.hasSelection = true;
    view.selectionStart = 0;
    view.selectionEnd = 96000;

    // Lane closed: nothing happens, and the caller is free to let Delete mean
    // something else.
    CHECK_FALSE(dave::gui::deleteAutomationInSelection(edit, undo, view));

    view.expandedTracks.insert(t);
    view.hasSelection = false;
    CHECK_FALSE(dave::gui::deleteAutomationInSelection(edit, undo, view));

    // Open lane, real selection, but no points inside it.
    view.hasSelection = true;
    view.selectionStart = 480000;
    view.selectionEnd = 960000;
    CHECK_FALSE(dave::gui::deleteAutomationInSelection(edit, undo, view));
    CHECK(edit.volumeAutomation(t)->size() == 1);
}

// ─── Eraser tool ─────────────────────────────────────────────────────────────

TEST_CASE("the eraser removes points inside its band and keeps the rest",
          "[automation]") {
    using dave::document::VolumeAutomationPoint;
    // A five-point envelope; the swept band [90000, 210000] covers b, c and d
    // and must leave a and e untouched, in order.
    std::vector<VolumeAutomationPoint> points{
        {"a", 0, 0.0},       {"b", 96000, -6.0},  {"c", 144000, -3.0},
        {"d", 192000, -9.0}, {"e", 288000, -1.0},
    };

    const auto kept = dave::gui::automationErase(points, 90000, 210000);
    REQUIRE(kept.size() == 2);
    CHECK(kept[0].id == "a");
    CHECK(kept[0].sample == 0);
    CHECK(kept[1].id == "e");
    CHECK(kept[1].sample == 288000);
}

TEST_CASE("the eraser band is inclusive at both edges", "[automation]") {
    using dave::document::VolumeAutomationPoint;
    std::vector<VolumeAutomationPoint> points{
        {"lo", 1000, 0.0}, {"mid", 2000, 0.0}, {"hi", 3000, 0.0},
    };
    // Points exactly on the boundaries count as inside.
    const auto kept = dave::gui::automationErase(points, 1000, 3000);
    CHECK(kept.empty());

    const auto keptOne = dave::gui::automationErase(points, 1001, 2999);
    REQUIRE(keptOne.size() == 2);
    CHECK(keptOne[0].id == "lo");
    CHECK(keptOne[1].id == "hi");
}

TEST_CASE("an eraser band touching no points changes nothing", "[automation]") {
    using dave::document::VolumeAutomationPoint;
    std::vector<VolumeAutomationPoint> points{
        {"a", 0, 0.0}, {"b", 48000, -6.0},
    };
    const auto kept = dave::gui::automationErase(points, 100000, 200000);
    CHECK(kept.size() == 2);
    CHECK(kept == points);
}
