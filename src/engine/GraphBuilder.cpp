// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/GraphBuilder.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <cstdio>
#include <unordered_set>

namespace dave::engine {

std::unique_ptr<Graph> GraphBuilder::build(const document::Edit& edit, double sampleRate) {
    auto graph = std::make_unique<Graph>();

    master_.reset();
    trackGains_.clear();
    clipNodes_.clear();

    // Master: the root of the derived graph.
    master_ = std::make_shared<GainNode>();
    master_->setGain(0.8);
    auto masterId = graph->addNode(master_);

    const auto& tracks = edit.tracks();
    auto masterSum = std::make_shared<SummingNode>(static_cast<int>(tracks.size()));
    auto masterSumId = graph->addNode(masterSum);
    graph->connect(masterSumId, 0, masterId, 0);

    int trackIndex = 0;
    for (const auto& track : tracks) {
        // Track gain (post-sum). Created even for empty tracks so the mixer
        // shows them and the master sum has the right pin count.
        auto gain = std::make_shared<GainNode>();
        gain->setGain(track.gain);
        gain->setPan(track.pan);
        auto gainId = graph->addNode(gain);
        trackGains_[track.id] = gain;

        // The "chain source" is what feeds the plugin chain (and ultimately the
        // track gain). For a track with clips it's the clip summing node; for
        // an empty track (clips but possibly plugins) we still need a node to
        // anchor the chain, so we use a silent SummingNode with 0 inputs.
        // (Previously this `continue`d on empty clips, which skipped the plugin
        // chain entirely — a bug that meant plugins on clip-less tracks never
        // loaded and Edit failed.)
        NodeId chainSource;
        std::shared_ptr<SummingNode> trackSum;
        if (track.clips.empty()) {
            // Silent source: a sum with no inputs produces silence. Plugins on
            // an empty track thus process silence (audible only once a clip is
            // added, but the chain + instance exist so Edit works immediately).
            trackSum = std::make_shared<SummingNode>(1);
            chainSource = graph->addNode(trackSum);
        } else {
            trackSum = std::make_shared<SummingNode>(static_cast<int>(track.clips.size()));
            chainSource = graph->addNode(trackSum);

            int clipIdx = 0;
            for (const auto& clip : track.clips) {
                const auto* asset = edit.asset(clip.asset);
                if (asset == nullptr) continue;

            // Cache the decoded buffer by asset id so re-derives don't reload.
            auto cacheIt = assetCache_.buffers.find(asset->id.sha256);
            if (cacheIt == assetCache_.buffers.end()) {
                drwav wav;
                if (!drwav_init_file(&wav, asset->path.c_str(), nullptr)) {
                    std::fprintf(stderr, "GraphBuilder: failed to open %s\n", asset->path.c_str());
                    continue;
                }
                std::vector<std::vector<float>> deinterleaved(wav.channels);
                for (auto& ch : deinterleaved) ch.resize(wav.totalPCMFrameCount);
                std::vector<float> interleaved(size_t(wav.totalPCMFrameCount) * wav.channels);
                drwav_uint64 decoded = drwav_read_pcm_frames_f32(
                    &wav, wav.totalPCMFrameCount, interleaved.data());
                for (drwav_uint64 i = 0; i < decoded; ++i) {
                    for (drwav_uint32 c = 0; c < wav.channels; ++c) {
                        deinterleaved[c][i] = interleaved[size_t(i) * wav.channels + c];
                    }
                }
                assetCache_.sampleRates[asset->id.sha256] = wav.sampleRate;
                assetCache_.channels[asset->id.sha256] = wav.channels;
                bool _unused;
                std::tie(cacheIt, _unused) =
                    assetCache_.buffers.emplace(asset->id.sha256, std::move(deinterleaved));
                drwav_uninit(&wav);
            }

            auto node = std::make_shared<AudioClipNode>();
            node->setBuffer(cacheIt->second, assetCache_.sampleRates[asset->id.sha256]);
            node->setStart(clip.timelineStart);
            node->setSourceOffset(clip.sourceOffset);
            node->setLength(clip.length);
            node->setFades(clip.fadeIn, clip.fadeOut);
            auto nodeId = graph->addNode(node);
            clipNodes_[clip.id] = node;
            graph->connect(nodeId, 0, chainSource, clipIdx);
                ++clipIdx;
            }
        } // end else (track has clips)

        // Build the plugin chain: chainSource → plugin[0] → ... → plugin[n-1] → gain.
        // chainSource was set above (clip sum, or silent sum for empty tracks).
        for (const auto& slot : track.plugins) {
            if (slot.bypass) continue; // skip bypassed plugins

            // Get-or-create the cached PluginInstance for this slot.
            auto& inst = pluginInstances_[slot.id];
            if (!inst) {
                inst = std::make_shared<PluginInstance>();
                PluginDescriptor desc;
                desc.name = slot.name;
                desc.path = slot.path;
                desc.uidString = slot.uidString;
                if (!inst->load(desc, sampleRate, 256)) {
                    std::fprintf(stderr, "Dave: failed to load plugin '%s': %s\n",
                                 slot.name.c_str(), inst->lastError().c_str());
                    inst.reset();
                    continue;
                }
                // Restore saved parameter state if present (RB-7 persistence).
                if (!slot.stateBase64.empty()) {
                    inst->setStateBase64(slot.stateBase64);
                }
            }

            auto node = std::make_shared<PluginNode>(inst);
            auto nodeId = graph->addNode(node);
            graph->connect(chainSource, 0, nodeId, 0);
            chainSource = nodeId;
        }

        graph->connect(chainSource, 0, gainId, 0);
        graph->connect(gainId, 0, masterSumId, trackIndex);
        ++trackIndex;
    }

    // Prune cached plugin instances for slots that no longer exist (the slot
    // was removed from the Edit). Collect live slot ids first.
    std::unordered_set<std::string> liveSlots;
    for (const auto& track : edit.tracks()) {
        for (const auto& slot : track.plugins) {
            liveSlots.insert(slot.id);
        }
    }
    for (auto it = pluginInstances_.begin(); it != pluginInstances_.end();) {
        if (liveSlots.count(it->first) == 0) {
            if (it->second) it->second->unload();
            it = pluginInstances_.erase(it);
        } else {
            ++it;
        }
    }

    return graph;
}

GraphBuilder::~GraphBuilder() = default;

} // namespace dave::engine
