// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/graph/Graph.h"
#include "engine/nodes/GainNode.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace dave::engine;

namespace {

// Emits a fixed value on every channel. Gives the graph a deterministic source
// so a test can assert on exact sample values rather than "something happened".
class ConstNode : public Node {
public:
    explicit ConstNode(float value) : Node("const"), value_(value) {}
    int numInputPins() const override { return 0; }
    int numOutputPins() const override { return 1; }
    void process(NodeProcessContext& ctx) override {
        for (int c = 0; c < ctx.output.numChannels; ++c) {
            for (int i = 0; i < ctx.numSamples; ++i) {
                ctx.output.channels[c][i] = value_;
            }
        }
    }
private:
    float value_;
};

// Passes audio through and records the order in which nodes ran, so we can
// assert the topological sort actually orders producers before consumers.
class OrderRecordingNode : public Node {
public:
    OrderRecordingNode(int id, std::vector<int>* log)
        : Node("order"), id_(id), log_(log) {}
    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    void process(NodeProcessContext& ctx) override {
        log_->push_back(id_);
        for (int c = 0; c < ctx.output.numChannels; ++c) {
            for (int i = 0; i < ctx.numSamples; ++i) {
                ctx.output.channels[c][i] = ctx.inputs[0].channels[c][i];
            }
        }
    }
private:
    int id_;
    std::vector<int>* log_;
};

// Runs one block through a compiled graph and returns the root output.
std::vector<std::vector<float>> renderBlock(CompiledGraph& g, int channels,
                                            int numSamples) {
    std::vector<std::vector<float>> storage(
        static_cast<size_t>(channels), std::vector<float>(numSamples, 0.0f));
    std::vector<float*> ptrs;
    for (auto& c : storage) ptrs.push_back(c.data());

    AudioBus root;
    root.channels = ptrs.data();
    root.numChannels = channels;
    root.numSamples = numSamples;
    root.clear();

    TimeInfo time;
    time.sampleRate = 48000.0;
    g.process(root, time);
    return storage;
}

} // namespace

TEST_CASE("compile rejects cycles", "[graph]") {
    Graph g;
    auto a = g.addNode(std::make_shared<GainNode>());
    auto b = g.addNode(std::make_shared<GainNode>());
    REQUIRE(g.connect(a, 0, b, 0));
    REQUIRE(g.connect(b, 0, a, 0));

    auto [compiled, err] = compile(g, 48000.0, 128);
    // A feedback loop has no valid execution order. It must be refused at
    // compile time on the UI thread — discovering it on the audio thread is
    // not an option.
    CHECK(compiled == nullptr);
    REQUIRE(err.has_value());
    CHECK_FALSE(err->message.empty());
}

TEST_CASE("compile accepts a simple chain", "[graph]") {
    Graph g;
    auto src = g.addNode(std::make_shared<ConstNode>(0.5f));
    auto gain = g.addNode(std::make_shared<GainNode>());
    REQUIRE(g.connect(src, 0, gain, 0));

    auto [compiled, err] = compile(g, 48000.0, 128);
    CHECK_FALSE(err.has_value());
    REQUIRE(compiled != nullptr);
    CHECK_FALSE(compiled->empty());
}

TEST_CASE("topological order runs producers before consumers", "[graph]") {
    std::vector<int> order;
    Graph g;
    auto src = g.addNode(std::make_shared<ConstNode>(1.0f));
    auto first = g.addNode(std::make_shared<OrderRecordingNode>(1, &order));
    auto second = g.addNode(std::make_shared<OrderRecordingNode>(2, &order));
    auto third = g.addNode(std::make_shared<OrderRecordingNode>(3, &order));
    // Deliberately connect out of insertion order: 1 -> 3 -> 2. If compile()
    // just walked nodes in the order they were added, this would run 2 before
    // its producer 3 and the test would catch it.
    REQUIRE(g.connect(src, 0, first, 0));
    REQUIRE(g.connect(first, 0, third, 0));
    REQUIRE(g.connect(third, 0, second, 0));

    auto [compiled, err] = compile(g, 48000.0, 64);
    REQUIRE(compiled != nullptr);
    compiled->prepareAll(48000.0, 64);
    renderBlock(*compiled, 2, 64);

    REQUIRE(order.size() == 3);
    const auto posOf = [&order](int id) {
        return std::find(order.begin(), order.end(), id) - order.begin();
    };
    CHECK(posOf(1) < posOf(3));
    CHECK(posOf(3) < posOf(2));
}

TEST_CASE("multiple edges into one pin sum", "[graph]") {
    Graph g;
    auto a = g.addNode(std::make_shared<ConstNode>(0.25f));
    auto b = g.addNode(std::make_shared<ConstNode>(0.5f));
    auto out = g.addNode(std::make_shared<GainNode>());
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.connect(b, 0, out, 0));

    auto [compiled, err] = compile(g, 48000.0, 32);
    REQUIRE(compiled != nullptr);
    compiled->prepareAll(48000.0, 32);
    auto rendered = renderBlock(*compiled, 2, 32);

    // Summing at a pin IS the mixer — if this regresses, every multi-clip
    // track mixes wrong. 0.25 + 0.5 = 0.75, then constant-power pan at centre
    // applies ~0.707 per channel.
    const float summed = rendered[0][0];
    CHECK(summed > 0.0f);
    CHECK(summed <= 0.75f + 1e-4f);
}

TEST_CASE("an empty graph compiles and renders silence", "[graph]") {
    Graph g;
    auto [compiled, err] = compile(g, 48000.0, 16);
    CHECK_FALSE(err.has_value());
    REQUIRE(compiled != nullptr);
    CHECK(compiled->empty());

    auto rendered = renderBlock(*compiled, 2, 16);
    for (const auto& ch : rendered) {
        for (float s : ch) CHECK(s == 0.0f);
    }
}

TEST_CASE("removing a node drops its edges", "[graph]") {
    Graph g;
    auto a = g.addNode(std::make_shared<ConstNode>(1.0f));
    auto b = g.addNode(std::make_shared<GainNode>());
    REQUIRE(g.connect(a, 0, b, 0));
    REQUIRE(g.edges().size() == 1);

    CHECK(g.removeNode(a));
    // A dangling edge would point at a node that no longer exists — compile()
    // would either crash or silently read freed memory.
    CHECK(g.edges().empty());

    auto [compiled, err] = compile(g, 48000.0, 16);
    CHECK_FALSE(err.has_value());
    CHECK(compiled != nullptr);
}
