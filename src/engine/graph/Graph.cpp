// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/graph/Graph.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace dave::engine {

// ─── Graph (UI-thread mutable) ──────────────────────────────────────────────

NodeId Graph::addNode(std::shared_ptr<Node> node) {
    NodeId id = nextId_++;
    nodes_.emplace_back(id, std::move(node));
    return id;
}

bool Graph::removeNode(NodeId id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [id](const auto& p) { return p.first == id; });
    if (it == nodes_.end()) {
        return false;
    }
    nodes_.erase(it);
    // Drop any edge touching this node.
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                 [id](const Edge& e) {
                                     return e.srcNode == id || e.dstNode == id;
                                 }),
                 edges_.end());
    return true;
}

bool Graph::connect(NodeId srcNode, int srcPin, NodeId dstNode, int dstPin) {
    auto* src = node(srcNode);
    auto* dst = node(dstNode);
    if (src == nullptr || dst == nullptr) {
        return false;
    }
    if (srcPin < 0 || srcPin >= src->numOutputPins()) {
        return false;
    }
    if (dstPin < 0 || dstPin >= dst->numInputPins()) {
        return false;
    }
    // No duplicate edges.
    for (const auto& e : edges_) {
        if (e.srcNode == srcNode && e.srcPin == srcPin &&
            e.dstNode == dstNode && e.dstPin == dstPin) {
            return false;
        }
    }
    edges_.push_back({srcNode, srcPin, dstNode, dstPin});
    return true;
}

bool Graph::disconnect(NodeId srcNode, int srcPin, NodeId dstNode, int dstPin) {
    auto it = std::find_if(edges_.begin(), edges_.end(),
                           [&](const Edge& e) {
                               return e.srcNode == srcNode && e.srcPin == srcPin &&
                                      e.dstNode == dstNode && e.dstPin == dstPin;
                           });
    if (it == edges_.end()) {
        return false;
    }
    edges_.erase(it);
    return true;
}

Node* Graph::node(NodeId id) const {
    auto sp = sharedNode(id);
    return sp ? sp.get() : nullptr;
}

std::shared_ptr<Node> Graph::sharedNode(NodeId id) const {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [id](const auto& p) { return p.first == id; });
    return it == nodes_.end() ? nullptr : it->second;
}

// ─── CompiledGraph ──────────────────────────────────────────────────────────

CompiledGraph::~CompiledGraph() = default;
CompiledGraph::CompiledGraph(CompiledGraph&&) noexcept = default;
CompiledGraph& CompiledGraph::operator=(CompiledGraph&&) noexcept = default;

void CompiledGraph::prepareAll(double sampleRate, int maxBlock) {
    for (auto& cn : nodes_) {
        if (cn.owned) {
            cn.owned->prepare(sampleRate, maxBlock);
        }
    }
}

void CompiledGraph::process(AudioBus& rootOutput, const AudioBus& hardwareInput,
                            const TimeInfo& time) {
    // --- RT thread --------------------------------------------------------
    // Walk nodes in topological order. For each node:
    //   1. For each input pin, zero its scratch channels, then sum each source
    //      node's output pin into them (mixing).
    //   2. Clear this node's output buffer.
    //   3. Call process().
    // All scratch is pre-allocated in compile() — NO allocation here.
    // Clamp rather than trust. Every buffer below is exactly maxBlock_ long,
    // and the host's block size is a request to the driver, not a guarantee
    // from it — see AudioEngine::kMaxBlock, which slices to match. If the two
    // ever disagree, losing the tail of a block is survivable; writing past
    // these buffers is not.
    const int n = std::min(rootOutput.numSamples, maxBlock_);
    const int chansPerPin = 2; // RB-1 fixed

    for (size_t i = 0; i < nodes_.size(); ++i) {
        auto& cn = nodes_[i];

        // Build input buses for this node from pre-allocated scratch + pointers.
        for (int pin = 0; pin < static_cast<int>(cn.inputSources.size()); ++pin) {
            for (int c = 0; c < chansPerPin; ++c) {
                size_t scratchIdx = cn.inputScratchIndex[pin][c];
                float* dst = inputScratch_[scratchIdx].data();
                std::memset(dst, 0, n * sizeof(float));
            }
            // Sum each source's output pin into this pin's scratch channels.
            for (auto& source : cn.inputSources[pin]) {
                auto& src = nodes_[source.nodeIndex];
                for (int c = 0; c < chansPerPin; ++c) {
                    size_t scratchIdx = cn.inputScratchIndex[pin][c];
                    float* dst = inputScratch_[scratchIdx].data();
                    const float* srcCh =
                        src.outChannels[source.sourcePin * chansPerPin + c];
                    if (dst != nullptr && srcCh != nullptr) {
                        if (source.delaySamples == 0) {
                            for (int s = 0; s < n; ++s) dst[s] += srcCh[s];
                        } else {
                            float* delay = source.delayStorage.data() +
                                static_cast<size_t>(c) * source.delaySamples;
                            size_t write = source.writePosition;
                            for (int s = 0; s < n; ++s) {
                                dst[s] += delay[write];
                                delay[write] = srcCh[s];
                                if (++write == source.delaySamples) write = 0;
                            }
                        }
                    }
                }
                if (source.delaySamples != 0) {
                    source.writePosition =
                        (source.writePosition + static_cast<size_t>(n)) %
                        source.delaySamples;
                }
            }
        }

        // Refresh input bus sample counts (channels/pointers already wired at
        // compile; only numSamples changes per block if block size varies).
        for (auto& bus : cn.inputBuses) {
            bus.numSamples = n;
        }

        // Clear this node's output buffer, then process.
        for (float* outCh : cn.outChannels) {
            if (outCh != nullptr) {
                std::memset(outCh, 0, n * sizeof(float));
            }
        }

        NodeProcessContext ctx{};
        ctx.numSamples = n;
        ctx.sampleRate = time.sampleRate;
        ctx.time = &time;
        ctx.inputs = cn.inputBuses.data();
        ctx.numInputs = static_cast<int>(cn.inputBuses.size());
        ctx.hardwareInput = &hardwareInput;
        // Output bus: pointers + sizes already set at compile; refresh samples.
        // cn.outputBus is reconstructed each block to keep the struct POD-ish;
        // it borrows cn.outChannels.data() (stable post-compile).
        AudioBus outBus;
        outBus.channels = cn.outChannels.data();
        outBus.numChannels = static_cast<int>(cn.outChannels.size());
        outBus.numSamples = n;
        ctx.output = outBus;

        cn.node->process(ctx);
    }

    // Copy the root node's output into the host's output buffer.
    if (!nodes_.empty() && rootIndex_ < nodes_.size()) {
        auto& root = nodes_[rootIndex_];
        int copyChans = std::min(static_cast<int>(root.outChannels.size()),
                                 rootOutput.numChannels);
        for (int c = 0; c < copyChans; ++c) {
            if (root.outChannels[c] != nullptr) {
                std::memcpy(rootOutput.channels[c], root.outChannels[c], n * sizeof(float));
            }
        }
    }
}

// ─── compile() ──────────────────────────────────────────────────────────────

std::pair<std::unique_ptr<CompiledGraph>, std::optional<CompileError>>
compile(const Graph& graph, double sampleRate, int maxBlock) {
    auto result = std::make_unique<CompiledGraph>();
    const auto& gnodes = graph.nodes();
    const auto& gedges = graph.edges();

    if (gnodes.empty()) {
        return {std::move(result), std::nullopt}; // empty graph is valid (silence)
    }

    // Map Graph NodeId -> index into gnodes, for edge resolution.
    std::unordered_map<NodeId, size_t> idToIndex;
    for (size_t i = 0; i < gnodes.size(); ++i) {
        idToIndex[gnodes[i].first] = i;
    }

    // Kahn's algorithm for topological sort + cycle detection.
    // Build adjacency: for each node, its out-neighbors (via edges) and
    // in-degree.
    std::vector<std::vector<size_t>> outNeighbors(gnodes.size());
    std::vector<int> inDegree(gnodes.size(), 0);

    // Also record, per (dst node, dst pin), the list of (src node, src pin)
    // for input gathering at RT time.
    std::vector<std::vector<std::vector<std::pair<size_t, int>>>> inputSources(
        gnodes.size());
    for (size_t i = 0; i < gnodes.size(); ++i) {
        inputSources[i].resize(gnodes[i].second->numInputPins());
    }

    for (const auto& e : gedges) {
        auto srcIt = idToIndex.find(e.srcNode);
        auto dstIt = idToIndex.find(e.dstNode);
        if (srcIt == idToIndex.end() || dstIt == idToIndex.end()) {
            return {nullptr, CompileError{"edge references unknown node"}};
        }
        size_t si = srcIt->second;
        size_t di = dstIt->second;
        outNeighbors[si].push_back(di);
        inDegree[di]++;
        if (e.dstPin < static_cast<int>(inputSources[di].size())) {
            inputSources[di][e.dstPin].emplace_back(si, e.srcPin);
        }
    }

    std::queue<size_t> q;
    for (size_t i = 0; i < gnodes.size(); ++i) {
        if (inDegree[i] == 0) {
            q.push(i);
        }
    }

    std::vector<size_t> order;
    order.reserve(gnodes.size());
    while (!q.empty()) {
        size_t cur = q.front();
        q.pop();
        order.push_back(cur);
        for (size_t nb : outNeighbors[cur]) {
            if (--inDegree[nb] == 0) {
                q.push(nb);
            }
        }
    }

    if (order.size() != gnodes.size()) {
        return {nullptr, CompileError{"cycle detected: graph must be a DAG"}};
    }

    // Resolve the root into a topo-order index. Default: the last node, which
    // is what process() used before roots were nameable — so a graph that
    // never calls setRoot() keeps its old behaviour exactly.
    result->maxBlock_ = maxBlock;
    result->rootIndex_ = order.empty() ? 0 : order.size() - 1;
    if (graph.root() != 0) {
        auto rootIt = idToIndex.find(graph.root());
        if (rootIt == idToIndex.end()) {
            return {nullptr, CompileError{"root references unknown node"}};
        }
        for (size_t topo = 0; topo < order.size(); ++topo) {
            if (order[topo] == rootIt->second) {
                result->rootIndex_ = topo;
                break;
            }
        }
    }

    // Allocate per-node output buffers and build CompiledNodes in topo order.
    const int chansPerPin = 2;
    result->nodes_.reserve(order.size());
    size_t scratchCounter = 0; // flat index into inputScratch_

    // Map from graph-node index (into gnodes) -> topo-order index (into
    // result->nodes_). inputSources above stored graph-node indices; we
    // translate them to topo-order indices below so RT-time lookups index the
    // right node. (This was the root cause of clip audio being dropped: the
    // sum node was reading from the wrong source node.)
    std::vector<size_t> graphIdxToTopo(gnodes.size(), SIZE_MAX);
    for (size_t ord = 0; ord < order.size(); ++ord) {
        graphIdxToTopo[order[ord]] = ord;
    }

    // Calculate the arrival time at every node output. Parallel edges feeding
    // the same input pin are aligned to the slowest source on that pin. Pins
    // stay independent, which prevents one hardware output pair from delaying
    // another merely because both are packed by the root node.
    std::vector<uint64_t> outputLatency(gnodes.size(), 0);
    std::vector<std::vector<uint64_t>> pinLatency(gnodes.size());
    for (size_t ord = 0; ord < order.size(); ++ord) {
        const size_t idx = order[ord];
        pinLatency[idx].resize(inputSources[idx].size(), 0);
        uint64_t latestInput = 0;
        for (size_t pin = 0; pin < inputSources[idx].size(); ++pin) {
            for (const auto& source : inputSources[idx][pin]) {
                pinLatency[idx][pin] =
                    std::max(pinLatency[idx][pin], outputLatency[source.first]);
            }
            latestInput = std::max(latestInput, pinLatency[idx][pin]);
        }
        outputLatency[idx] = latestInput + gnodes[idx].second->latencySamples();
    }

    const uint64_t maxDelaySamples = static_cast<uint64_t>(
        std::max(0.0, sampleRate) * 10.0);
    constexpr uint64_t kMaxDelayBytes = 256ULL * 1024ULL * 1024ULL;
    uint64_t totalDelayBytes = 0;

    // First pass: allocate the output buffer pool and count total input-pins
    // so we can size inputScratch_ precisely (one slot per (node,pin,channel)).
    size_t totalInputSlots = 0;
    for (size_t ord = 0; ord < order.size(); ++ord) {
        size_t idx = order[ord];
        totalInputSlots += gnodes[idx].second->numInputPins() * chansPerPin;
    }
    result->inputScratch_.resize(totalInputSlots, std::vector<float>(maxBlock, 0.0f));

    for (size_t ord = 0; ord < order.size(); ++ord) {
        size_t idx = order[ord];
        const auto& [nid, nodePtr] = gnodes[idx];

        CompiledGraph::CompiledNode cn;
        // SHARE ownership (don't steal). The Graph stays alive and editable on
        // the UI thread; the CompiledGraph holds a shared_ptr so the node
        // survives until the RT thread has swapped to a new compiled graph AND
        // the old one is freed on the UI thread. Node objects are mutated only
        // via atomic members (RT-safe), so shared access is safe.
        cn.owned = nodePtr;
        cn.node = cn.owned.get();
        cn.inputSources.resize(inputSources[idx].size());
        for (size_t pin = 0; pin < inputSources[idx].size(); ++pin) {
            for (const auto& source : inputSources[idx][pin]) {
                const uint64_t delay =
                    pinLatency[idx][pin] - outputLatency[source.first];
                if (delay > maxDelaySamples) {
                    return {nullptr, CompileError{
                        "delay compensation exceeds 10 seconds on one path"}};
                }
                const uint64_t bytes = delay * 2ULL * sizeof(float);
                if (bytes > kMaxDelayBytes - totalDelayBytes) {
                    return {nullptr, CompileError{
                        "delay compensation exceeds 256 MiB storage budget"}};
                }
                totalDelayBytes += bytes;
                CompiledGraph::CompiledNode::InputSource compiledSource;
                compiledSource.nodeIndex = graphIdxToTopo[source.first];
                compiledSource.sourcePin = source.second;
                compiledSource.delaySamples = static_cast<uint32_t>(delay);
                compiledSource.delayStorage.assign(
                    static_cast<size_t>(delay) * 2, 0.0f);
                cn.inputSources[pin].push_back(std::move(compiledSource));
            }
        }

        // Allocate output channels: numOutputPins * chansPerPin.
        int totalOutChans = cn.node->numOutputPins() * chansPerPin;
        cn.outChannels.resize(totalOutChans, nullptr);
        for (int c = 0; c < totalOutChans; ++c) {
            // Output buffers live in their own storage, owned by the node's
            // vector. We don't pool them (simpler; pooling is a later opt).
            cn.outChannels[c] = nullptr; // will allocate below
        }
        // Allocate per-node output backing storage inline.
        cn.outStorage.assign(static_cast<size_t>(totalOutChans) * maxBlock, 0.0f);
        for (int c = 0; c < totalOutChans; ++c) {
            cn.outChannels[c] = cn.outStorage.data() + static_cast<size_t>(c) * maxBlock;
        }

        // Wire up input buses: one AudioBus per input pin, each pointing at
        // chansPerPin scratch slots from the pool.
        int numInPins = cn.node->numInputPins();
        cn.inputBuses.resize(numInPins);
        cn.inputBusChannels.resize(numInPins);
        cn.inputScratchIndex.resize(numInPins);
        for (int pin = 0; pin < numInPins; ++pin) {
            cn.inputBusChannels[pin].resize(chansPerPin);
            cn.inputScratchIndex[pin].resize(chansPerPin);
            for (int c = 0; c < chansPerPin; ++c) {
                cn.inputScratchIndex[pin][c] = scratchCounter++;
                cn.inputBusChannels[pin][c] =
                    result->inputScratch_[cn.inputScratchIndex[pin][c]].data();
            }
            cn.inputBuses[pin].channels = cn.inputBusChannels[pin].data();
            cn.inputBuses[pin].numChannels = chansPerPin;
            cn.inputBuses[pin].numSamples = maxBlock; // refreshed per-block in process()
        }

        result->nodes_.push_back(std::move(cn));
    }

    result->prepareAll(sampleRate, maxBlock);
    return {std::move(result), std::nullopt};
}

} // namespace dave::engine
