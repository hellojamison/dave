// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "editing/Commands.h"
#include "engine/GraphBuilder.h"
#include "engine/graph/Graph.h"
#include "engine/nodes/RoutingNodes.h"
#include "engine/nodes/SummingNode.h"
#include "gui/RoutingViewModel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <memory>
#include <vector>

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

using namespace dave;

namespace {

class ImpulseNode final : public engine::Node {
public:
    ImpulseNode() : Node("impulse") {}
    void process(engine::NodeProcessContext& context) override {
        for (int channel = 0; channel < context.output.numChannels; ++channel) {
            context.output.channels[channel][0] = 1.0f;
        }
    }
};

class LatentNode final : public engine::Node {
public:
    explicit LatentNode(uint32_t latency) : Node("latent"), latency_(latency) {}
    int numInputPins() const override { return 1; }
    uint32_t latencySamples() const override { return latency_; }
    void prepare(double, int) override {
        storage_.assign(static_cast<size_t>(latency_) * 2, 0.0f);
        position_ = 0;
    }
    void process(engine::NodeProcessContext& context) override {
        if (latency_ == 0) return;
        for (int sample = 0; sample < context.numSamples; ++sample) {
            for (int channel = 0; channel < 2; ++channel) {
                float& delayed = storage_[static_cast<size_t>(channel) * latency_ + position_];
                context.output.channels[channel][sample] = delayed;
                delayed = context.inputs[0].channels[channel][sample];
            }
            if (++position_ == latency_) position_ = 0;
        }
    }
private:
    uint32_t latency_ = 0;
    size_t position_ = 0;
    std::vector<float> storage_;
};

std::vector<std::vector<float>> render(engine::CompiledGraph& graph, int channels,
                                       int frames,
                                       const std::vector<std::vector<float>>& input = {}) {
    std::vector<std::vector<float>> output(
        static_cast<size_t>(channels), std::vector<float>(frames, 0.0f));
    std::vector<float*> outputPointers;
    for (auto& channel : output) outputPointers.push_back(channel.data());
    engine::AudioBus outputBus{outputPointers.data(), channels, frames};

    std::vector<float*> inputPointers;
    for (const auto& channel : input) {
        inputPointers.push_back(const_cast<float*>(channel.data()));
    }
    engine::AudioBus inputBus{inputPointers.data(),
                              static_cast<int>(inputPointers.size()), frames};
    engine::TimeInfo time;
    graph.process(outputBus, inputBus, time);
    return output;
}

} // namespace

TEST_CASE("new edits own a permanent Main bus", "[routing][document]") {
    document::Edit edit;
    // Main is the only row a new edit owns, and it is a track like any
    // other — the permanence is the flag, not a separate list.
    REQUIRE(edit.tracks().size() == 1);
    REQUIRE(userTracks(edit) == 0);
    REQUIRE(edit.mainBus() != nullptr);
    CHECK(edit.mainBus()->isMain);
    CHECK(edit.mainBus()->mainOutput == document::RouteTarget::hardwareOutput(0, 2));

    const auto audio = edit.addTrack("DX");
    const auto midi = edit.addMidiTrack("Score");
    CHECK(edit.track(audio)->mainOutput == document::RouteTarget::bus());
    CHECK(edit.track(midi)->mainOutput == document::RouteTarget::bus());
    CHECK_FALSE(edit.removeTrack(document::kMainBusId));
}

TEST_CASE("v1 projects migrate routing without enabling input monitoring",
          "[routing][document]") {
    const char* v1 = R"({
      "format":"dave.doc/v1",
      "tracks":[{"id":"a","name":"DX","inputChannel":3,"inputChannelCount":2}],
      "midiTracks":[{"id":"m","name":"Keys"}]
    })";
    document::Edit edit;
    REQUIRE(document::deserializeEdit(v1, edit).ok);
    REQUIRE(edit.mainBus() != nullptr);
    CHECK(edit.track("a")->hardwareInput == document::HardwareChannelSpan{3, 2});
    CHECK_FALSE(edit.track("a")->inputMonitor);
    CHECK(edit.track("a")->mainOutput == document::RouteTarget::bus());
    CHECK(edit.track("m")->mainOutput == document::RouteTarget::bus());
}

TEST_CASE("current documents preserve unavailable routes, buses, and muted sends",
          "[routing][document]") {
    document::Edit edit;
    const auto track = edit.addTrack("DX");
    const auto bus = edit.addBus("Print");
    REQUIRE(edit.setTrackHardwareInput(track, {31, 2}));
    REQUIRE(edit.setMainOutput(track, document::RouteTarget::bus(bus)));
    document::AuxSend send;
    send.target = document::RouteTarget::hardwareOutput(14, 2);
    const auto sendId = edit.addSend(track, send);
    REQUIRE_FALSE(sendId.empty());

    const std::string text = document::serializeEdit(edit);
    CHECK(text.find("dave.doc/v4") != std::string::npos);
    document::Edit loaded;
    REQUIRE(document::deserializeEdit(text, loaded).ok);
    CHECK(loaded.track(track)->hardwareInput == document::HardwareChannelSpan{31, 2});
    CHECK(loaded.track(track)->mainOutput == document::RouteTarget::bus(bus));
    REQUIRE(loaded.track(track)->sends.size() == 1);
    CHECK(loaded.track(track)->sends[0].id == sendId);
    CHECK(loaded.track(track)->sends[0].muted);
    CHECK(loaded.track(track)->sends[0].gain == 0.0);
}

TEST_CASE("routing rejects cycles, self routes, and referenced deletion",
          "[routing][document]") {
    document::Edit edit;
    const auto a = edit.addTrack("A");
    const auto b = edit.addTrack("B");
    const auto bus = edit.addBus("Stem");
    REQUIRE(edit.setMainOutput(a, document::RouteTarget::audioTrack(b)));
    CHECK_FALSE(edit.setMainOutput(b, document::RouteTarget::audioTrack(a)));
    CHECK_FALSE(edit.setMainOutput(a, document::RouteTarget::audioTrack(a)));
    REQUIRE(edit.setMainOutput(b, document::RouteTarget::bus(bus)));
    CHECK_FALSE(edit.removeTrack(bus));
    CHECK_FALSE(edit.removeTrack(b));
    CHECK(edit.validateRouting().ok);
}

TEST_CASE("bus and send commands preserve stable ids across undo redo",
          "[routing][commands]") {
    document::Edit edit;
    editing::UndoStack undo(edit);
    const auto track = edit.addTrack("A");
    auto addBus = std::make_unique<editing::AddTrackCommand>("Stem");
    auto* addBusPointer = addBus.get();
    undo.execute(std::move(addBus));
    const std::string busId = addBusPointer->busId();
    REQUIRE(edit.track(busId) != nullptr);
    undo.undo();
    CHECK(edit.track(busId) == nullptr);
    undo.redo();
    REQUIRE(edit.track(busId) != nullptr);

    document::AuxSend send;
    send.target = document::RouteTarget::bus(busId);
    auto addSend = std::make_unique<editing::AddSendCommand>(track, send);
    auto* addSendPointer = addSend.get();
    undo.execute(std::move(addSend));
    const std::string sendId = addSendPointer->sendId();
    REQUIRE(edit.track(track)->sends[0].id == sendId);
    undo.undo();
    CHECK(edit.track(track)->sends.empty());
    undo.redo();
    REQUIRE(edit.track(track)->sends.size() == 1);
    CHECK(edit.track(track)->sends[0].id == sendId);
}

TEST_CASE("Main refuses software routes and sends", "[routing][document]") {
    document::Edit edit;
    const auto track = edit.addTrack("A");
    CHECK_FALSE(edit.setMainOutput(document::kMainBusId,
                                   document::RouteTarget::audioTrack(track)));
    document::AuxSend send;
    send.target = document::RouteTarget::audioTrack(track);
    CHECK(edit.addSend(document::kMainBusId, send).empty());
    CHECK(edit.validateRouting().ok);
}

TEST_CASE("hardware input selection duplicates mono and isolates stereo",
          "[routing][graph]") {
    engine::Graph monoGraph;
    const auto mono = monoGraph.addNode(
        std::make_shared<engine::HardwareInputNode>(1, 1));
    monoGraph.setRoot(mono);
    auto [compiledMono, monoError] = engine::compile(monoGraph, 48000.0, 8);
    REQUIRE_FALSE(monoError.has_value());
    std::vector<std::vector<float>> hardware(3, std::vector<float>(8, 0.0f));
    hardware[1][0] = 0.75f;
    const auto monoOutput = render(*compiledMono, 2, 8, hardware);
    CHECK(monoOutput[0][0] == 0.75f);
    CHECK(monoOutput[1][0] == 0.75f);

    engine::Graph stereoGraph;
    const auto stereo = stereoGraph.addNode(
        std::make_shared<engine::HardwareInputNode>(1, 2));
    stereoGraph.setRoot(stereo);
    auto [compiledStereo, stereoError] = engine::compile(stereoGraph, 48000.0, 8);
    REQUIRE_FALSE(stereoError.has_value());
    hardware[2][0] = -0.25f;
    const auto stereoOutput = render(*compiledStereo, 2, 8, hardware);
    CHECK(stereoOutput[0][0] == 0.75f);
    CHECK(stereoOutput[1][0] == -0.25f);
}

TEST_CASE("hardware routes map stereo pairs and mono folds", "[routing][graph]") {
    engine::Graph graph;
    const auto source = graph.addNode(std::make_shared<ImpulseNode>());
    const auto pairRoute = graph.addNode(
        std::make_shared<engine::HardwareRouteNode>(0, 2));
    const auto monoRoute = graph.addNode(
        std::make_shared<engine::HardwareRouteNode>(1, 1));
    const auto root = graph.addNode(std::make_shared<engine::HardwareOutputNode>(2));
    REQUIRE(graph.connect(source, 0, pairRoute, 0));
    REQUIRE(graph.connect(source, 0, monoRoute, 0));
    REQUIRE(graph.connect(pairRoute, 0, root, 0));
    REQUIRE(graph.connect(monoRoute, 0, root, 1));
    graph.setRoot(root);
    auto [compiled, error] = engine::compile(graph, 48000.0, 8);
    REQUIRE_FALSE(error.has_value());
    const auto output = render(*compiled, 4, 8);
    CHECK(output[0][0] == 1.0f);
    CHECK(output[1][0] == 1.0f);
    CHECK(output[2][0] == 0.0f);
    CHECK(output[3][0] == Catch::Approx(1.41421356f));
}

TEST_CASE("PDC aligns parallel latent edges at a summing pin", "[routing][pdc]") {
    engine::Graph graph;
    const auto direct = graph.addNode(std::make_shared<ImpulseNode>());
    const auto latentSource = graph.addNode(std::make_shared<ImpulseNode>());
    const auto latent = graph.addNode(std::make_shared<LatentNode>(5));
    const auto sum = graph.addNode(std::make_shared<engine::SummingNode>(1));
    REQUIRE(graph.connect(latentSource, 0, latent, 0));
    REQUIRE(graph.connect(direct, 0, sum, 0));
    REQUIRE(graph.connect(latent, 0, sum, 0));
    graph.setRoot(sum);
    auto [compiled, error] = engine::compile(graph, 48000.0, 16);
    REQUIRE_FALSE(error.has_value());
    const auto output = render(*compiled, 2, 16);
    for (int sample = 0; sample < 5; ++sample) CHECK(output[0][sample] == 0.0f);
    CHECK(output[0][5] == 2.0f);
}

TEST_CASE("PDC refuses excessive per-path compensation", "[routing][pdc]") {
    engine::Graph graph;
    const auto direct = graph.addNode(std::make_shared<ImpulseNode>());
    const auto source = graph.addNode(std::make_shared<ImpulseNode>());
    const auto huge = graph.addNode(std::make_shared<LatentNode>(480001));
    const auto sum = graph.addNode(std::make_shared<engine::SummingNode>(1));
    REQUIRE(graph.connect(source, 0, huge, 0));
    REQUIRE(graph.connect(direct, 0, sum, 0));
    REQUIRE(graph.connect(huge, 0, sum, 0));
    auto [compiled, error] = engine::compile(graph, 48000.0, 16);
    CHECK(compiled == nullptr);
    REQUIRE(error.has_value());
    CHECK(error->message.find("10 seconds") != std::string::npos);
}

TEST_CASE("PDC refuses aggregate delay storage above 256 MiB", "[routing][pdc]") {
    engine::Graph graph;
    const auto direct = graph.addNode(std::make_shared<ImpulseNode>());
    const auto source = graph.addNode(std::make_shared<ImpulseNode>());
    // A deliberately high synthetic rate lets one legal (< 10 second) edge
    // exceed the storage budget without allocating hundreds of MiB in a test.
    const auto latent = graph.addNode(std::make_shared<LatentNode>(40000000));
    const auto sum = graph.addNode(std::make_shared<engine::SummingNode>(1));
    REQUIRE(graph.connect(source, 0, latent, 0));
    REQUIRE(graph.connect(direct, 0, sum, 0));
    REQUIRE(graph.connect(latent, 0, sum, 0));
    auto [compiled, error] = engine::compile(graph, 10000000.0, 16);
    CHECK(compiled == nullptr);
    REQUIRE(error.has_value());
    CHECK(error->message.find("256 MiB") != std::string::npos);
}

TEST_CASE("PDC keeps independent hardware output pairs independently timed",
          "[routing][pdc]") {
    engine::Graph graph;
    const auto direct = graph.addNode(std::make_shared<ImpulseNode>());
    const auto source = graph.addNode(std::make_shared<ImpulseNode>());
    const auto latent = graph.addNode(std::make_shared<LatentNode>(5));
    const auto root = graph.addNode(std::make_shared<engine::HardwareOutputNode>(2));
    REQUIRE(graph.connect(source, 0, latent, 0));
    REQUIRE(graph.connect(direct, 0, root, 0));
    REQUIRE(graph.connect(latent, 0, root, 1));
    graph.setRoot(root);
    auto [compiled, error] = engine::compile(graph, 48000.0, 16);
    REQUIRE_FALSE(error.has_value());
    const auto output = render(*compiled, 4, 16);
    CHECK(output[0][0] == 1.0f);
    CHECK(output[1][0] == 1.0f);
    CHECK(output[2][0] == 0.0f);
    CHECK(output[3][0] == 0.0f);
    CHECK(output[2][5] == 1.0f);
    CHECK(output[3][5] == 1.0f);
}

TEST_CASE("routing target menus group destinations and disable cycles",
          "[routing][ui]") {
    document::Edit edit;
    const auto a = edit.addTrack("A");
    const auto b = edit.addTrack("B");
    const auto bus = edit.addBus("Stem");
    REQUIRE(edit.setMainOutput(a, document::RouteTarget::audioTrack(b)));
    const auto options = gui::routingTargetOptions(edit, b, 4, false);
    const auto cycle = std::find_if(options.begin(), options.end(), [&](const auto& option) {
        return option.target == document::RouteTarget::audioTrack(a);
    });
    REQUIRE(cycle != options.end());
    CHECK_FALSE(cycle->enabled);
    CHECK_FALSE(cycle->disabledReason.empty());
    CHECK(std::any_of(options.begin(), options.end(), [&](const auto& option) {
        return option.group == gui::RoutingTargetOption::Group::MainAndBuses &&
               option.target == document::RouteTarget::bus(bus);
    }));
    CHECK(std::any_of(options.begin(), options.end(), [&](const auto& option) {
        return option.group == gui::RoutingTargetOption::Group::HardwareOutputs &&
               option.target == document::RouteTarget::hardwareOutput(2, 2);
    }));
}

TEST_CASE("routing view model preserves unavailable labels and queues requests",
          "[routing][ui]") {
    document::Edit edit;
    const auto track = edit.addTrack("A");
    const auto unavailable = document::RouteTarget::hardwareOutput(8, 2);
    CHECK(gui::routeTargetLabel(edit, unavailable, 2) ==
          "Output 9-10 (unavailable)");

    gui::RoutingViewModel view;
    gui::RoutingRequest request;
    request.kind = gui::RoutingRequest::Kind::SetMainOutput;
    request.ownerId = track;
    request.route = unavailable;
    view.request(request);
    REQUIRE(view.requests().size() == 1);
    const auto taken = view.takeRequests();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].route == unavailable);
    CHECK(view.requests().empty());
}

TEST_CASE("GraphBuilder renders monitored input through nested buses",
          "[routing][render]") {
    document::Edit edit;
    const auto track = edit.addTrack("DX");
    const auto stem = edit.addBus("Stem");
    REQUIRE(edit.setInputMonitor(track, true));
    CHECK(edit.track(track)->hardwareInput == document::HardwareChannelSpan{0, 1});
    REQUIRE(edit.setMainOutput(track, document::RouteTarget::bus(stem)));
    REQUIRE(edit.setMainOutput(document::kMainBusId,
                               document::RouteTarget::hardwareOutput(2, 2)));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 4);
    auto [compiled, error] = engine::compile(*graph, 48000.0, 16);
    REQUIRE_FALSE(error.has_value());
    std::vector<std::vector<float>> input(1, std::vector<float>(16, 0.0f));
    input[0][0] = 1.0f;
    const auto output = render(*compiled, 4, 16, input);
    CHECK(output[0][0] == 0.0f);
    CHECK(output[1][0] == 0.0f);
    CHECK(output[2][0] > 0.0f);
    CHECK(output[3][0] > 0.0f);
}

TEST_CASE("pre and post sends tap opposite sides of the fader",
          "[routing][render]") {
    document::Edit edit;
    const auto trackId = edit.addTrack("DX");
    auto* track = edit.track(trackId);
    REQUIRE(track != nullptr);
    track->gain = 0.5;
    REQUIRE(edit.setInputMonitor(trackId, true));
    document::AuxSend pre;
    pre.target = document::RouteTarget::hardwareOutput(2, 1);
    pre.tap = document::SendTap::PreFader;
    pre.gain = 1.0;
    pre.muted = false;
    REQUIRE_FALSE(edit.addSend(trackId, pre).empty());
    document::AuxSend post;
    post.target = document::RouteTarget::hardwareOutput(3, 1);
    post.tap = document::SendTap::PostFader;
    post.gain = 1.0;
    post.muted = false;
    REQUIRE_FALSE(edit.addSend(trackId, post).empty());

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 4);
    auto [compiled, error] = engine::compile(*graph, 48000.0, 16);
    REQUIRE_FALSE(error.has_value());
    std::vector<std::vector<float>> input(1, std::vector<float>(16, 0.0f));
    input[0][0] = 1.0f;
    const auto output = render(*compiled, 4, 16, input);
    CHECK(output[2][0] == Catch::Approx(1.41421356f));
    CHECK(output[3][0] == Catch::Approx(0.5f));
}

TEST_CASE("solo-in-place keeps the required path and silences sibling routes",
          "[routing][solo]") {
    document::Edit edit;
    const auto trackId = edit.addTrack("DX");
    REQUIRE(edit.setInputMonitor(trackId, true));
    document::AuxSend cue;
    cue.target = document::RouteTarget::hardwareOutput(2, 2);
    cue.gain = 1.0;
    cue.muted = false;
    REQUIRE_FALSE(edit.addSend(trackId, cue).empty());
    edit.track(document::kMainBusId)->solo = true;

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 4);
    auto [compiled, error] = engine::compile(*graph, 48000.0, 16);
    REQUIRE_FALSE(error.has_value());
    std::vector<std::vector<float>> input(1, std::vector<float>(16, 0.0f));
    input[0][0] = 1.0f;
    const auto output = render(*compiled, 4, 16, input);
    CHECK(output[0][0] > 0.0f);
    CHECK(output[1][0] > 0.0f);
    CHECK(output[2][0] == 0.0f);
    CHECK(output[3][0] == 0.0f);
}

TEST_CASE("a v3 project folds its three bands into one ordered list",
          "[routing][document][migration]") {
    // The failure this guards is the one that would silently damage real
    // sessions: rows arriving in the wrong order, Main not last, or a bus
    // losing its hardware output because readChannelRouting defaults every
    // channel to Main.
    const std::string v3 = R"({
        "format": "dave.doc/v3",
        "sampleRate": 48000,
        "bitDepth": 24,
        "tracks": [{"id":"track_1","name":"Dialog"},
                   {"id":"track_2","name":"FX"}],
        "midiTracks": [{"id":"miditrack_1","name":"Keys"}],
        "buses": [{"id":"bus_9","name":"Print"},
                  {"id":"bus_main","name":"Main","isMain":true}]
    })";
    document::Edit edit;
    REQUIRE(document::deserializeEdit(v3, edit).ok);

    // Audio band, then MIDI, then buses — the order the user saw.
    REQUIRE(edit.tracks().size() == 5);
    CHECK(edit.tracks()[0].id == "track_1");
    CHECK(edit.tracks()[1].id == "track_2");
    CHECK(edit.tracks()[2].id == "miditrack_1");
    CHECK(edit.tracks()[3].id == "bus_9");
    CHECK(edit.tracks()[4].id == "bus_main");

    // Main is last, permanent, and still routed to hardware rather than to
    // itself.
    CHECK(edit.tracks().back().isMain);
    REQUIRE(edit.mainBus() != nullptr);
    CHECK(edit.mainBus()->mainOutput.kind ==
          document::RouteTarget::Kind::HardwareOutput);
    // isMain is derived from the id, never trusted from the file.
    CHECK_FALSE(edit.tracks()[3].isMain);

    // And it re-saves as v4 with one array.
    const std::string saved = document::serializeEdit(edit);
    CHECK(saved.find("dave.doc/v4") != std::string::npos);
    CHECK(saved.find("\"midiTracks\"") == std::string::npos);
    CHECK(saved.find("\"buses\"") == std::string::npos);
}

TEST_CASE("removing a track a send points at is refused, whatever it holds",
          "[routing][document]") {
    // removeMidiTrack had no routeReferences guard, so deleting a MIDI track
    // a send referenced left a dangling route. One track type, one guard.
    document::Edit edit;
    const std::string keys = edit.addMidiTrack("Keys");
    const std::string source = edit.addTrack("Dialog");
    document::AuxSend send;
    send.target = document::RouteTarget::audioTrack(keys);
    REQUIRE_FALSE(edit.addSend(source, send).empty());

    CHECK_FALSE(edit.removeTrack(keys));
    CHECK(edit.track(keys) != nullptr);

    // Main is permanent regardless of references.
    CHECK_FALSE(edit.removeTrack(document::kMainBusId));
    CHECK(edit.mainBus() != nullptr);
}

TEST_CASE("plugins can be added to any track, including one made for MIDI",
          "[routing][document]") {
    // addPlugin resolved track-or-bus and silently missed MIDI tracks, which
    // is the entire reason addMidiPlugin existed.
    document::Edit edit;
    const std::string keys = edit.addMidiTrack("Keys");
    document::PluginSlot slot;
    slot.name = "EQ";
    slot.uidString = "uid-eq";
    const std::string id = edit.addPlugin(keys, slot);
    REQUIRE_FALSE(id.empty());
    REQUIRE(edit.track(keys)->plugins.size() == 1);
    CHECK(edit.removePlugin(keys, id));
    CHECK(edit.track(keys)->plugins.empty());
}

TEST_CASE("moving a send through the chain moves where it taps",
          "[routing][render]") {
    // The order in the list has to be the order in the signal, or the chain is
    // decoration. Pre/post-fader is no longer a property of the send — it is
    // whether the send sits before or after the Fader entry.
    document::Edit edit;
    const auto trackId = edit.addTrack("DX");
    auto* track = edit.track(trackId);
    REQUIRE(track != nullptr);
    track->gain = 0.5;
    REQUIRE(edit.setInputMonitor(trackId, true));

    document::AuxSend send;
    send.target = document::RouteTarget::hardwareOutput(2, 1);
    send.gain = 1.0;
    send.muted = false;
    const std::string sendId = edit.addSend(trackId, send);
    REQUIRE_FALSE(sendId.empty());

    const auto tapLevel = [&]() {
        engine::GraphBuilder builder;
        auto graph = builder.build(edit, 48000.0, 4);
        auto [compiled, error] = engine::compile(*graph, 48000.0, 16);
        REQUIRE_FALSE(error.has_value());
        std::vector<std::vector<float>> input(1, std::vector<float>(16, 0.0f));
        input[0][0] = 1.0f;
        return render(*compiled, 4, 16, input)[2][0];
    };

    // Default placement is after the fader, so the 0.5 fader scales it.
    CHECK(tapLevel() == Catch::Approx(0.5f));

    // Find the send and the fader, and put the send ahead of the fader.
    const auto& chain = edit.track(trackId)->chain;
    size_t sendRow = chain.size();
    size_t faderRow = chain.size();
    for (size_t i = 0; i < chain.size(); ++i) {
        if (chain[i].kind == document::ChainSlot::Kind::Send) sendRow = i;
        if (chain[i].kind == document::ChainSlot::Kind::Fader) faderRow = i;
    }
    REQUIRE(sendRow < chain.size());
    REQUIRE(faderRow < chain.size());
    REQUIRE(edit.moveChainSlot(trackId, sendRow, faderRow));

    // Now it taps ahead of the fader and the fader no longer affects it.
    CHECK(tapLevel() == Catch::Approx(1.41421356f));
}

TEST_CASE("the chain meter reads the level arriving at the fader",
          "[routing][render]") {
    // The fader's row is drawn as a meter, and the point of putting it there
    // is that it shows the level you are setting the fader AGAINST. A tap on
    // the far side would show the result instead, which is the one thing it
    // must not do — you would be reading your own fader move.
    document::Edit edit;
    const auto trackId = edit.addTrack("DX");
    auto* track = edit.track(trackId);
    REQUIRE(track != nullptr);
    track->gain = 0.5;
    REQUIRE(edit.setInputMonitor(trackId, true));

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 2);
    auto [compiled, error] = engine::compile(*graph, 48000.0, 16);
    REQUIRE_FALSE(error.has_value());

    std::vector<std::vector<float>> input(1, std::vector<float>(16, 1.0f));
    (void)render(*compiled, 2, 16, input);

    REQUIRE(builder.meterTaps().count(trackId) == 1);
    auto tap = builder.meterTaps().at(trackId);
    REQUIRE(tap != nullptr);
    // Full scale at the tap, with the 0.5 fader still ahead of it.
    CHECK(tap->meter(0, true).peak == Catch::Approx(1.0f).margin(0.01f));

    // And the track's own gain node sees the same signal on its input, which
    // is what makes the two readings comparable.
    REQUIRE(builder.trackGains().count(trackId) == 1);
    CHECK(builder.trackGains().at(trackId)->meter(0, true).peak ==
          Catch::Approx(1.0f).margin(0.01f));
    // Post-fader is where the halving shows up.
    CHECK(builder.trackGains().at(trackId)->meter(0, false).peak < 0.75f);
}
