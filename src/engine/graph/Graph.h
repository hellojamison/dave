// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Node.h"
#include "engine/graph/Types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dave::engine {

// A node's identity within a graph. Stable across recompiles as long as the
// node isn't removed. (RB-1: simple incrementing IDs; stable IDs for undo come
// with the document model in RB-2.)
using NodeId = uint64_t;

// An edge connects a source node's output pin to a destination node's input
// pin. Multiple edges into one (dst, dstPin) sum at that pin — that's mixing.
struct Edge {
    NodeId srcNode;
    int srcPin;
    NodeId dstNode;
    int dstPin;
};

// Graph is the mutable, UI-thread representation of the signal graph. The UI
// (and the routing editor) build/modify this; compile() turns it into an
// immutable CompiledGraph for the RT thread. Graph owns its nodes.
//
// Threading: ALL methods are UI-thread-only. Never touch from the RT thread —
// use the CompiledGraph that compile() returns.
class Graph {
public:
    Graph() = default;

    // Add a node; returns its NodeId. The Graph shares ownership — caller may
    // keep a handle for parameter edits (e.g. a UI knob holding the node's
    // shared_ptr) while the Graph also holds one.
    NodeId addNode(std::shared_ptr<Node> node);

    // Remove a node (and any edges connected to it). Returns true if found.
    bool removeNode(NodeId id);

    // Connect src output pin to dst input pin. Returns false if either node/pin
    // doesn't exist or the connection would be invalid.
    bool connect(NodeId srcNode, int srcPin, NodeId dstNode, int dstPin);

    // Disconnect a specific edge. Returns true if it existed.
    bool disconnect(NodeId srcNode, int srcPin, NodeId dstNode, int dstPin);

    // Name the node whose output reaches the speakers. Without this, compile()
    // falls back to whichever node happens to sort last, which is only the
    // master by luck — any sink with no outgoing edge (a meter tap, a recorder,
    // a hardware output) can sort after it and silently become the output.
    // Setting an unknown id is ignored; compile() falls back as before.
    void setRoot(NodeId id) { rootNode_ = id; }
    NodeId root() const { return rootNode_; }

    Node* node(NodeId id) const;
    std::shared_ptr<Node> sharedNode(NodeId id) const;
    const std::vector<std::pair<NodeId, std::shared_ptr<Node>>>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }

private:
    std::vector<std::pair<NodeId, std::shared_ptr<Node>>> nodes_;
    std::vector<Edge> edges_;
    NodeId nextId_ = 1;
    // 0 = unset; compile() then uses the last node in topological order.
    NodeId rootNode_ = 0;
};

// Result of compile(): either a ready-to-run CompiledGraph or an error
// (cycle, dangling edge, etc.).
struct CompileError {
    std::string message;
};

// CompiledGraph is the immutable execution plan the RT thread walks each block.
// Built by compile() on the UI thread; published to the RT thread via the
// AudioEngine's atomic swap. All buffers are allocated in compile() and freed
// in the destructor — the RT thread never allocates.
class CompiledGraph {
public:
    CompiledGraph() = default;
    ~CompiledGraph();

    CompiledGraph(const CompiledGraph&) = delete;
    CompiledGraph& operator=(const CompiledGraph&) = delete;
    CompiledGraph(CompiledGraph&&) noexcept;
    CompiledGraph& operator=(CompiledGraph&&) noexcept;

    // Prepare every node for the given sample rate / block size. Called by
    // compile() and by the AudioEngine on device changes.
    void prepareAll(double sampleRate, int maxBlock);

    // RT thread entry point. Walks nodes in topological order, gathering each
    // node's inputs (summed per pin) and calling node->process().
    void process(AudioBus& rootOutput, const AudioBus& hardwareInput,
                 const TimeInfo& time);
    void process(AudioBus& rootOutput, const TimeInfo& time) {
        const AudioBus empty{};
        process(rootOutput, empty, time);
    }

    bool empty() const { return nodes_.empty(); }

    // The largest block this graph was compiled for. Every node's buffers are
    // exactly this long, so handing process() more than this would write past
    // them. process() clamps rather than trusting the caller: a short block is
    // a glitch, an overrun is heap corruption.
    int maxBlock() const { return maxBlock_; }

private:
    friend std::pair<std::unique_ptr<CompiledGraph>, std::optional<CompileError>>
    compile(const Graph&, double, int);

    // One entry per node, in topological execution order.
    struct CompiledNode {
        Node* node;                       // borrowed (Graph owns it during compile)
        std::shared_ptr<Node> owned;      // ownership transferred here after compile
        std::vector<float> outStorage;    // backing storage for outChannels
        std::vector<float*> outChannels;  // pointers into outStorage
        // For each input pin: list of (source compiled-node index, source pin)
        struct InputSource {
            size_t nodeIndex = 0;
            int sourcePin = 0;
            uint32_t delaySamples = 0;
            size_t writePosition = 0;
            std::vector<float> delayStorage;
        };
        std::vector<std::vector<InputSource>> inputSources;
        // Pre-allocated input bus structures for RT: per pin, the AudioBus and
        // its channel pointer array. Pointers point into inputScratch_ pool.
        // These are reused every block — NO allocation in process().
        std::vector<AudioBus> inputBuses;
        std::vector<std::vector<float*>> inputBusChannels; // [pin][channel]->ptr
        // Flat indices into inputScratch_ for each (pin, channel).
        std::vector<std::vector<size_t>> inputScratchIndex; // [pin][channel]->pool idx
    };

    std::vector<CompiledNode> nodes_;
    int maxBlock_ = 0;
    // Which entry of nodes_ feeds the host output. Resolved in compile() from
    // Graph::root(); defaults to the last node so a graph that never named one
    // behaves exactly as it did before roots existed.
    size_t rootIndex_ = 0;
    // Pool of sample buffers for summed inputs. Sized precisely in compile():
    // one slot per (node, input-pin, channel). Owned here, never resized post-
    // compile. RT-safe.
    std::vector<std::vector<float>> inputScratch_;
};

// Compile a Graph into a CompiledGraph. Performs:
//   1. Cycle detection (rejects feedback loops).
//   2. Topological sort (Kahn's algorithm).
//   3. Buffer allocation.
//   4. Per-node input-edge grouping (for summed pins).
// On success returns {graph, nullopt}. On failure returns {nullptr, error}.
std::pair<std::unique_ptr<CompiledGraph>, std::optional<CompileError>>
compile(const Graph& graph, double sampleRate, int maxBlock);

} // namespace dave::engine
