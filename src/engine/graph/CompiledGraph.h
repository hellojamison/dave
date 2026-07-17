#pragma once

#include "engine/graph/Node.h"

#include <memory>
#include <vector>

namespace dave::engine {

// CompiledGraph is the immutable execution plan the RT thread walks each block.
// It is built on the UI thread by the (Phase 1+) Graph compiler and swapped
// into place at a block boundary via an atomic pointer. For Phase 0 it just
// holds an ordered list of nodes — enough to prove the contract with a single
// SineNode.
//
// The RT contract: process() here allocates nothing and locks nothing. All
// nodes must already be prepared.
class CompiledGraph {
public:
    // Nodes are held as shared_ptr so the UI thread can keep a handle to a
    // node (for parameter edits) while the graph owns it. The graph itself is
    // immutable once published to the RT thread; only the node objects' atomic
    // state is mutated after publish.
    void addNode(std::shared_ptr<Node> node) {
        nodes_.push_back(std::move(node));
    }

    // Prepare every node. Called on the UI thread before the graph goes live.
    void prepareAll(double sampleRate, int maxBlock) {
        for (auto& n : nodes_)
            n->prepare(sampleRate, maxBlock);
    }

    // RT thread entry point. Walks nodes in compiled order.
    void process(ProcessContext& ctx) {
        for (auto& n : nodes_)
            n->process(ctx);
    }

    void releaseAll() {
        for (auto& n : nodes_)
            n->release();
    }

    bool empty() const { return nodes_.empty(); }

private:
    std::vector<std::shared_ptr<Node>> nodes_;
};

} // namespace dave::engine
