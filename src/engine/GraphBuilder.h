// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"
#include "engine/graph/Graph.h"
#include "engine/nodes/AudioClipNode.h"
#include "engine/nodes/GainNode.h"
#include "engine/nodes/SummingNode.h"
#include "engine/plugins/PluginInstance.h"
#include "engine/plugins/PluginNode.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace dave::engine {

// GraphBuilder derives an engine::Graph from a document::Edit. This is the
// single bridge between the user's track/clip/plugin model and the RT graph.
//
// Derivation rule (per track):
//   clips → trackSum → plugin[0] → plugin[1] → ... → trackGain → masterSum → master
// Tracks with no plugins skip the plugin chain (trackSum → trackGain directly).
// A free-routing editor (Reaper-style grid) replaces this fixed rule later;
// GraphBuilder is the seam where that plugs in.
//
// Caching: asset decodes AND plugin instances are cached across re-derives
// (keyed by asset id and plugin slot id respectively). Creating a PluginInstance
// is expensive (load + initialize the VST3), so we keep instances alive when
// the slot still exists, and drop them when the slot is removed.
class GraphBuilder {
public:
    GraphBuilder() = default;
    ~GraphBuilder();

    // Build a fresh engine::Graph from the given Edit.
    std::unique_ptr<Graph> build(const document::Edit& edit, double sampleRate);

    std::shared_ptr<GainNode> master() const { return master_; }
    const std::unordered_map<std::string, std::shared_ptr<GainNode>>& trackGains() const {
        return trackGains_;
    }
    const std::unordered_map<std::string, std::shared_ptr<AudioClipNode>>& clipNodes() const {
        return clipNodes_;
    }
    const std::unordered_map<std::string, std::vector<std::vector<float>>>& assetBuffers() const {
        return assetCache_.buffers;
    }

    // Look up the live PluginInstance for a slot id (or nullptr if not built
    // yet / slot was removed). The UI uses this to open a plugin's editor.
    std::shared_ptr<PluginInstance> pluginInstance(const std::string& slotId) const {
        auto it = pluginInstances_.find(slotId);
        return it == pluginInstances_.end() ? nullptr : it->second;
    }

private:
    struct AssetCache {
        std::unordered_map<std::string, std::vector<std::vector<float>>> buffers;
        std::unordered_map<std::string, double> sampleRates;
        std::unordered_map<std::string, int> channels;
    };
    AssetCache assetCache_;

    // Plugin instances keyed by slot id. Survive re-derives; pruned of slots
    // no longer in the Edit at the start of each build().
    std::unordered_map<std::string, std::shared_ptr<PluginInstance>> pluginInstances_;

    std::shared_ptr<GainNode> master_;
    std::unordered_map<std::string, std::shared_ptr<GainNode>> trackGains_;
    std::unordered_map<std::string, std::shared_ptr<AudioClipNode>> clipNodes_;
};

} // namespace dave::engine
