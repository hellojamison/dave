// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "engine/GraphBuilder.h"
#include "engine/nodes/GainNode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <memory>

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
    CHECK(json.find("dave.doc/v3") != std::string::npos);
    dave::document::Edit loaded;
    REQUIRE(dave::document::deserializeEdit(json, loaded).ok);
    REQUIRE(loaded.track(audio)->volumeAutomation.size() == 1);
    REQUIRE(loaded.midiTrack(midi)->volumeAutomation.size() == 1);
    REQUIRE(loaded.bus(bus)->volumeAutomation.size() == 1);
    REQUIRE(loaded.mainBus()->volumeAutomation.size() == 1);
    CHECK(loaded.track(audio)->volumeAutomation[0].db == Approx(-3.0));
    CHECK(loaded.midiTrack(midi)->volumeAutomation[0].db == Approx(-6.0));
    CHECK(loaded.bus(bus)->volumeAutomation[0].db == Approx(3.0));
    REQUIRE(loaded.track(audio)->panAutomation.size() == 1);
    REQUIRE(loaded.midiTrack(midi)->panAutomation.size() == 1);
    REQUIRE(loaded.bus(bus)->panAutomation.size() == 1);
    REQUIRE(loaded.mainBus()->panAutomation.size() == 1);
    CHECK(loaded.track(audio)->panAutomation[0].pan == Approx(-0.75));
    CHECK(loaded.midiTrack(midi)->panAutomation[0].pan == Approx(0.25));
    CHECK(loaded.bus(bus)->panAutomation[0].pan == Approx(1.0));
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
