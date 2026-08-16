// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/DecodedAudioAsset.h"
#include "document/Edit.h"
#include "engine/graph/Graph.h"
#include "engine/nodes/AudioClipNode.h"
#include "engine/nodes/GainNode.h"
#include "engine/nodes/InstrumentNode.h"
#include "engine/nodes/RoutingNodes.h"
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
// Derivation rule (per audio track):
//   clips → trackSum → plugin[0] → plugin[1] → ... → trackGain → masterSum → master
// Tracks with no plugins skip the plugin chain (trackSum → trackGain directly).
//
// Per MIDI track:
//   instrumentNode → plugin[0] → ... → trackGain → masterSum → master
// The InstrumentNode is a generator that owns both the baked note sequence and
// the instrument plugin, so a MIDI track differs from an audio track only in
// what sits at the head of the chain.
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
    std::unique_ptr<Graph> build(const document::Edit& edit, double sampleRate,
                                 int playbackChannels = 2);

    std::shared_ptr<GainNode> master() const { return master_; }
    const std::unordered_map<std::string, std::shared_ptr<GainNode>>& trackGains() const {
        return trackGains_;
    }
    // The chain-position meter taps, keyed by track id. One per track, wherever
    // Track::meterTapIndex put it — read through its PRE bank, since a tap
    // passes audio through and its "post" is the same signal.
    const std::unordered_map<std::string, std::shared_ptr<GainNode>>&
    meterTaps() const {
        return meterTaps_;
    }
    const std::unordered_map<std::string, std::shared_ptr<AudioClipNode>>& clipNodes() const {
        return clipNodes_;
    }
    // Instrument nodes keyed by MIDI track id (present even for tracks whose
    // instrument failed to load, so the mixer can still find the track).
    const std::unordered_map<std::string, std::shared_ptr<InstrumentNode>>&
    instrumentNodes() const {
        return instrumentNodes_;
    }
    const std::unordered_map<std::string, audio::DecodedAudioAssetPtr>& assetBuffers() const {
        return assetCache_.buffers;
    }
    audio::DecodedAudioAssetPtr decodedAsset(const std::string& assetId) const {
        const auto found = assetCache_.buffers.find(assetId);
        return found == assetCache_.buffers.end() ? nullptr : found->second;
    }

    // Look up the live PluginInstance for a slot id (or nullptr if not built
    // yet / slot was removed). The UI uses this to open a plugin's editor.
    std::shared_ptr<PluginInstance> pluginInstance(const std::string& slotId) const {
        auto it = pluginInstances_.find(slotId);
        return it == pluginInstances_.end() ? nullptr : it->second;
    }

    // UI-thread poll for VST3 latency changes. The app rebuilds the immutable
    // graph before the next publication when this returns true.
    bool consumeLatencyChange();
    bool latencyChangePending() const;

private:
    // Get-or-create the cached PluginInstance for a slot, restoring its saved
    // state on first load. Returns nullptr if the slot names no plugin or the
    // load failed. Shared by audio chains, MIDI chains, and instruments — one
    // cache means one place that decides when a plugin gets reloaded.
    std::shared_ptr<PluginInstance> instanceForSlot(const document::PluginSlot& slot,
                                                    double sampleRate);

    struct AssetCache {
        std::unordered_map<std::string, audio::DecodedAudioAssetPtr> buffers;
    };
    AssetCache assetCache_;

    // Plugin instances keyed by slot id. Survive re-derives; pruned of slots
    // no longer in the Edit at the start of each build().
    std::unordered_map<std::string, std::shared_ptr<PluginInstance>> pluginInstances_;
    std::unordered_map<std::string, uint32_t> pluginLatencies_;

    std::shared_ptr<GainNode> master_;
    std::unordered_map<std::string, std::shared_ptr<GainNode>> trackGains_;
    std::unordered_map<std::string, std::shared_ptr<GainNode>> meterTaps_;
    std::unordered_map<std::string, std::shared_ptr<AudioClipNode>> clipNodes_;
    std::unordered_map<std::string, std::shared_ptr<InstrumentNode>> instrumentNodes_;
};

} // namespace dave::engine
