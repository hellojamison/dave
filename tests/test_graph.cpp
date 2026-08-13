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

// ─── Explicit root ──────────────────────────────────────────────────────────

namespace {

// A sink: consumes audio, emits nothing. Kahn's algorithm can pop it last,
// which is exactly the hazard setRoot() exists to remove. Meter taps, hardware
// outputs and the recorder are all this shape.
class SinkNode : public Node {
public:
    SinkNode() : Node("sink") {}
    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    void process(NodeProcessContext& ctx) override {
        // Deliberately writes a value nobody should ever hear.
        for (int c = 0; c < ctx.output.numChannels; ++c) {
            for (int i = 0; i < ctx.numSamples; ++i) {
                ctx.output.channels[c][i] = -9.0f;
            }
        }
    }
};

// Copies input to output unchanged. GainNode would do, but its constant-power
// pan law is -3 dB at centre, which would make these assertions about the pan
// law rather than about which node reaches the output.
class PassThroughNode : public Node {
public:
    PassThroughNode() : Node("passthrough") {}
    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    void process(NodeProcessContext& ctx) override {
        for (int c = 0; c < ctx.output.numChannels; ++c) {
            for (int i = 0; i < ctx.numSamples; ++i) {
                ctx.output.channels[c][i] = ctx.inputs[0].channels[c][i];
            }
        }
    }
};

// Run a compiled graph for one block and return the first output sample.
float renderFirstSample(CompiledGraph& cg, int numSamples = 32) {
    std::vector<std::vector<float>> storage(2, std::vector<float>(numSamples, 0.0f));
    std::vector<float*> ptrs{storage[0].data(), storage[1].data()};
    AudioBus out;
    out.channels = ptrs.data();
    out.numChannels = 2;
    out.numSamples = numSamples;
    TimeInfo time{};
    time.sampleRate = 48000.0;
    cg.process(out, time);
    return storage[0][0];
}

} // namespace

TEST_CASE("a sink node added after the root does not steal the output",
          "[graph]") {
    // Without an explicit root, process() copies whichever node sorted last.
    // A sink has no outgoing edge, so it can land there and silence the mix.
    Graph g;
    auto srcId = g.addNode(std::make_shared<ConstNode>(0.5f));
    auto masterId = g.addNode(std::make_shared<PassThroughNode>());
    g.connect(srcId, 0, masterId, 0);
    g.setRoot(masterId);

    // Added last and consuming the master, so it sorts after it.
    auto sinkId = g.addNode(std::make_shared<SinkNode>());
    g.connect(masterId, 0, sinkId, 0);

    auto [cg, err] = compile(g, 48000.0, 64);
    REQUIRE_FALSE(err.has_value());
    REQUIRE(cg != nullptr);

    // The master passes 0.5 through; the sink's -9 must not reach the output.
    CHECK(renderFirstSample(*cg) == 0.5f);
}

TEST_CASE("a graph that never names a root keeps the old behaviour",
          "[graph]") {
    // Every existing call site predates setRoot(); they must be unaffected.
    Graph g;
    auto srcId = g.addNode(std::make_shared<ConstNode>(0.25f));
    auto masterId = g.addNode(std::make_shared<PassThroughNode>());
    g.connect(srcId, 0, masterId, 0);

    auto [cg, err] = compile(g, 48000.0, 64);
    REQUIRE_FALSE(err.has_value());
    CHECK(renderFirstSample(*cg) == 0.25f);
}

TEST_CASE("compiling with a root that isn't in the graph is an error",
          "[graph]") {
    // Silently falling back would hide a real wiring mistake.
    Graph g;
    g.addNode(std::make_shared<ConstNode>(1.0f));
    g.setRoot(9999);

    auto [cg, err] = compile(g, 48000.0, 64);
    CHECK(cg == nullptr);
    REQUIRE(err.has_value());
}

TEST_CASE("process clamps a block larger than the graph was compiled for",
          "[graph]") {
    // The device's block size is a request to the driver, not a promise from
    // it. Every node buffer is exactly maxBlock long, so an oversized block
    // used to write past them — a heap overflow that only showed up when the
    // driver changed its mind. AudioEngine slices to match; this is the net
    // under that, so a caller that gets it wrong glitches instead of corrupts.
    Graph g;
    auto srcId = g.addNode(std::make_shared<ConstNode>(1.0f));
    auto rootId = g.addNode(std::make_shared<PassThroughNode>());
    g.connect(srcId, 0, rootId, 0);
    g.setRoot(rootId);

    constexpr int kCompiledFor = 64;
    auto [cg, err] = compile(g, 48000.0, kCompiledFor);
    REQUIRE_FALSE(err.has_value());
    CHECK(cg->maxBlock() == kCompiledFor);

    // Hand it four times what it was built for, with the tail pre-filled with
    // a sentinel so we can see exactly how far it wrote.
    constexpr int kOversized = kCompiledFor * 4;
    std::vector<std::vector<float>> storage(2, std::vector<float>(kOversized, -1.0f));
    std::vector<float*> ptrs{storage[0].data(), storage[1].data()};
    AudioBus out;
    out.channels = ptrs.data();
    out.numChannels = 2;
    out.numSamples = kOversized;
    TimeInfo time{};
    time.sampleRate = 48000.0;
    cg->process(out, time);

    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < kCompiledFor; ++i) {
            INFO("channel " << c << " sample " << i);
            REQUIRE(storage[c][i] == 1.0f);
        }
        // Everything past the compiled size is untouched, not garbage.
        for (int i = kCompiledFor; i < kOversized; ++i) {
            INFO("channel " << c << " sample " << i);
            REQUIRE(storage[c][i] == -1.0f);
        }
    }
}
