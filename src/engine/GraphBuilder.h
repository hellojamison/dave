#pragma once

#include "document/Edit.h"
#include "engine/graph/Graph.h"
#include "engine/nodes/AudioClipNode.h"
#include "engine/nodes/GainNode.h"
#include "engine/nodes/SummingNode.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace dave::engine {

// GraphBuilder derives an engine::Graph from a document::Edit. This is the
// single bridge between the user's track/clip model and the RT graph.
//
// For RB-2 the derivation rule is fixed:
//   - Each audio clip → an AudioClipNode (loaded from the asset on first sight).
//   - All clips on a track → sum into a per-track GainNode (track gain/pan).
//   - All track gains → sum into a master GainNode (the root).
// A free-routing editor (Reaper-style grid) replaces this fixed rule in a
// later phase; the GraphBuilder is the seam where that plugs in.
//
// Asset loading: to avoid reloading the same WAV on every re-derive (e.g. when
// dragging an unrelated clip), GraphBuilder caches one AudioClipNode buffer
// per asset id. The node is reused; only its clipStart is updated.
class GraphBuilder {
public:
    // Build a fresh engine::Graph from the given Edit. Caches asset decodes
    // across calls (the cache lives in this builder, so keep the builder alive
    // across re-derives).
    std::unique_ptr<Graph> build(const document::Edit& edit, double sampleRate);

    // Master gain handle — set by build(); the UI can edit it afterward.
    std::shared_ptr<GainNode> master() const { return master_; }

    // Track-gain handles keyed by track id (for per-track faders).
    const std::unordered_map<std::string, std::shared_ptr<GainNode>>& trackGains() const {
        return trackGains_;
    }

    // Clip-node handles keyed by clip id (for live edits like position, though
    // position changes currently trigger a full re-derive).
    const std::unordered_map<std::string, std::shared_ptr<AudioClipNode>>& clipNodes() const {
        return clipNodes_;
    }

    // Read-only access to the decoded asset buffers, keyed by asset id (sha256).
    // The timeline/waveform renderer uses this to compute peaks without
    // re-decoding.
    const std::unordered_map<std::string, std::vector<std::vector<float>>>& assetBuffers() const {
        return assetCache_.buffers;
    }

private:
    // Cache of decoded audio per asset id, so a re-derive doesn't reload WAVs.
    struct AssetCache {
        std::unordered_map<std::string, std::vector<std::vector<float>>> buffers;
        std::unordered_map<std::string, double> sampleRates;
        std::unordered_map<std::string, int> channels;
    };
    AssetCache assetCache_;

    std::shared_ptr<GainNode> master_;
    std::unordered_map<std::string, std::shared_ptr<GainNode>> trackGains_;
    std::unordered_map<std::string, std::shared_ptr<AudioClipNode>> clipNodes_;
};

} // namespace dave::engine
