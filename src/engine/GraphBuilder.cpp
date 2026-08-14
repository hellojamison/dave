// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/AudioImportPolicy.h"
#include "engine/GraphBuilder.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <unordered_set>

namespace dave::engine {

bool GraphBuilder::latencyChangePending() const {
    for (const auto& [id, instance] : pluginInstances_) {
        (void)id;
        if (instance && instance->latencyChangePending()) return true;
    }
    return false;
}

bool GraphBuilder::consumeLatencyChange() {
    bool changed = false;
    for (const auto& [id, instance] : pluginInstances_) {
        if (!instance) continue;
        const bool notified = instance->consumeLatencyChange();
        const uint32_t latency = instance->latencySamples();
        auto [found, inserted] = pluginLatencies_.emplace(id, latency);
        if (!inserted && found->second != latency) {
            found->second = latency;
            changed = true;
        }
        changed = changed || notified;
    }
    return changed;
}

std::shared_ptr<PluginInstance> GraphBuilder::instanceForSlot(
    const document::PluginSlot& slot, double sampleRate) {
    if (slot.id.empty() || slot.uidString.empty()) return nullptr;

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
            return nullptr;
        }
        // Restore saved parameter state if present (RB-7 persistence).
        if (!slot.stateBase64.empty()) {
            inst->setStateBase64(slot.stateBase64);
        }
    }
    return inst;
}

std::unique_ptr<Graph> GraphBuilder::build(const document::Edit& edit,
                                           double sampleRate,
                                           int playbackChannels) {
    auto graph = std::make_unique<Graph>();
    master_.reset();
    trackGains_.clear();
    clipNodes_.clear();
    instrumentNodes_.clear();

    const int physicalChannels = std::max(1, playbackChannels);
    const int physicalPairs = (physicalChannels + 1) / 2;
    const auto hardwareRoot = std::make_shared<HardwareOutputNode>(physicalPairs);
    const NodeId hardwareRootId = graph->addNode(hardwareRoot);
    graph->setRoot(hardwareRootId);

    struct ChannelNodes {
        NodeId input = 0;
        NodeId preFader = 0;
        NodeId postFader = 0;
        bool muted = false;
    };
    std::unordered_map<std::string, ChannelNodes> channels;

    // Routing-aware solo: keep each explicitly soloed channel, its ancestors,
    // and its path to hardware alive. Mute is applied afterward and always wins.
    std::unordered_map<std::string, std::vector<std::string>> forward;
    std::unordered_map<std::string, std::vector<std::string>> reverse;
    auto targetKey = [](const document::RouteTarget& target) {
        if (target.kind == document::RouteTarget::Kind::AudioTrack) {
            return std::string("audio:") + target.targetId;
        }
        if (target.kind == document::RouteTarget::Kind::Bus) {
            return std::string("bus:") + target.targetId;
        }
        if (target.kind == document::RouteTarget::Kind::HardwareOutput) {
            return std::string("hardware:") +
                std::to_string(target.hardware.firstChannel) + ":" +
                std::to_string(target.hardware.channelCount);
        }
        return std::string("none");
    };
    auto rememberRoute = [&](const std::string& owner,
                             const document::RouteTarget& target) {
        const std::string destination = targetKey(target);
        if (destination == "none") return;
        forward[owner].push_back(destination);
        if (target.kind == document::RouteTarget::Kind::AudioTrack ||
            target.kind == document::RouteTarget::Kind::Bus) {
            reverse[target.targetId].push_back(owner);
        }
    };
    auto rememberChannel = [&](const auto& channel) {
        rememberRoute(channel.id, channel.mainOutput);
        for (const auto& send : channel.sends) {
            rememberRoute(channel.id, send.target);
        }
    };
    for (const auto& track : edit.tracks()) rememberChannel(track);
    for (const auto& track : edit.midiTracks()) rememberChannel(track);
    for (const auto& bus : edit.buses()) rememberChannel(bus);

    const bool anySoloed = edit.anySoloed();
    std::unordered_set<std::string> soloActive;
    std::unordered_set<std::string> soloEdges;
    auto edgeKey = [](const std::string& from, const std::string& to) {
        return from + "\n" + to;
    };
    auto activateSolo = [&](const std::string& id) {
        std::unordered_set<std::string> downstreamSeen;
        std::function<void(const std::string&)> downstream =
            [&](const std::string& current) {
                if (!downstreamSeen.insert(current).second) return;
                soloActive.insert(current);
                auto found = forward.find(current);
                if (found == forward.end()) return;
                for (const auto& next : found->second) {
                    soloEdges.insert(edgeKey(current, next));
                    if (next.starts_with("audio:")) downstream(next.substr(6));
                    else if (next.starts_with("bus:")) downstream(next.substr(4));
                }
            };
        downstream(id);
        std::unordered_set<std::string> upstreamSeen;
        std::function<void(const std::string&)> upstream = [&](const std::string& current) {
            if (!upstreamSeen.insert(current).second) return;
            soloActive.insert(current);
            auto found = reverse.find(current);
            if (found == reverse.end()) return;
            for (const auto& previous : found->second) {
                const std::string destination = edit.bus(current)
                    ? "bus:" + current : "audio:" + current;
                soloEdges.insert(edgeKey(previous, destination));
                upstream(previous);
            }
        };
        upstream(id);
    };
    for (const auto& track : edit.tracks()) if (track.solo) activateSolo(track.id);
    for (const auto& track : edit.midiTracks()) if (track.solo) activateSolo(track.id);
    for (const auto& bus : edit.buses()) if (bus.solo) activateSolo(bus.id);

    auto addPluginChain = [&](NodeId source,
                              const std::vector<document::PluginSlot>& plugins) {
        for (const auto& slot : plugins) {
            if (slot.bypass) continue;
            auto instance = instanceForSlot(slot, sampleRate);
            if (!instance) continue;
            const NodeId plugin = graph->addNode(
                std::make_shared<PluginNode>(std::move(instance)));
            graph->connect(source, 0, plugin, 0);
            source = plugin;
        }
        return source;
    };

    auto addGain = [&](const auto& channel, NodeId preFader) {
        auto gain = std::make_shared<GainNode>();
        const bool active = !anySoloed || soloActive.count(channel.id) != 0;
        gain->setGain(!channel.mute && active ? channel.gain : 0.0);
        gain->setPan(channel.pan);
        std::vector<GainNode::AutomationPoint> automation;
        automation.reserve(channel.volumeAutomation.size());
        for (const auto& point : channel.volumeAutomation) {
            automation.push_back({point.sample, point.db});
        }
        gain->setVolumeAutomation(std::move(automation));
        std::vector<GainNode::PanAutomationPoint> panAutomation;
        panAutomation.reserve(channel.panAutomation.size());
        for (const auto& point : channel.panAutomation) {
            panAutomation.push_back({point.sample, point.pan});
        }
        gain->setPanAutomation(std::move(panAutomation));
        const NodeId gainId = graph->addNode(gain);
        graph->connect(preFader, 0, gainId, 0);
        trackGains_[channel.id] = gain;
        channels[channel.id] = {0, preFader, gainId, channel.mute};
        return gainId;
    };

    // Create every receiver before connecting any document route.
    for (const auto& track : edit.tracks()) {
        const NodeId input = graph->addNode(std::make_shared<SummingNode>(1));
        channels[track.id].input = input;

        if (!track.clips.empty()) {
            const NodeId clips = graph->addNode(
                std::make_shared<SummingNode>(static_cast<int>(track.clips.size())));
            int clipPin = 0;
            for (const auto& clip : track.clips) {
                const auto* asset = edit.asset(clip.asset);
                if (!asset) continue;
                auto cache = assetCache_.buffers.find(asset->id.sha256);
                if (cache == assetCache_.buffers.end()) {
                    std::error_code sizeError;
                    const auto encodedBytes =
                        std::filesystem::file_size(asset->path, sizeError);
                    if (!sizeError && !audio::canDecodeFileInMemory(encodedBytes)) {
                        std::fprintf(stderr,
                                     "Dave: WAV import refused: files larger than 4 GiB "
                                     "require streaming playback: %s\n",
                                     asset->path.c_str());
                        continue;
                    }
                    drwav wav;
                    if (!drwav_init_file(&wav, asset->path.c_str(), nullptr)) continue;
                    auto decodedAsset = std::make_shared<audio::DecodedAudioAsset>();
                    decodedAsset->sampleRate = static_cast<double>(wav.sampleRate);
                    decodedAsset->channels.resize(wav.channels);
                    for (auto& channel : decodedAsset->channels) {
                        channel.resize(wav.totalPCMFrameCount);
                    }
                    std::vector<float> interleaved(
                        static_cast<size_t>(wav.totalPCMFrameCount) * wav.channels);
                    const drwav_uint64 decoded = drwav_read_pcm_frames_f32(
                        &wav, wav.totalPCMFrameCount, interleaved.data());
                    for (drwav_uint64 frame = 0; frame < decoded; ++frame) {
                        for (drwav_uint32 channel = 0; channel < wav.channels; ++channel) {
                            decodedAsset->channels[channel][frame] = interleaved[
                                static_cast<size_t>(frame) * wav.channels + channel];
                        }
                    }
                    bool inserted = false;
                    std::tie(cache, inserted) = assetCache_.buffers.emplace(
                        asset->id.sha256, std::move(decodedAsset));
                    (void)inserted;
                    drwav_uninit(&wav);
                }
                auto node = std::make_shared<AudioClipNode>();
                node->setBuffer(cache->second);
                node->setStart(clip.timelineStart);
                node->setSourceOffset(clip.sourceOffset);
                node->setLength(clip.length);
                node->setFades(clip.fadeIn, clip.fadeOut);
                const NodeId clipId = graph->addNode(node);
                clipNodes_[clip.id] = node;
                graph->connect(clipId, 0, clips, clipPin++);
            }
            graph->connect(clips, 0, input, 0);
        }

        if (track.inputMonitor) {
            const int first = track.inputChannel;
            const int count = std::clamp(track.inputChannelCount, 1, 2);
            const NodeId hardware = graph->addNode(
                std::make_shared<HardwareInputNode>(first, count));
            graph->connect(hardware, 0, input, 0);
        }
        const NodeId pre = addPluginChain(input, track.plugins);
        addGain(track, pre);
        channels[track.id].input = input;
    }

    for (const auto& track : edit.midiTracks()) {
        const NodeId input = graph->addNode(std::make_shared<SummingNode>(1));
        auto instrument = instanceForSlot(track.instrument, sampleRate);
        if (instrument) {
            auto node = std::make_shared<InstrumentNode>();
            node->setInstrument(std::move(instrument));
            node->setSequence(bakeClips(track.clips));
            const NodeId instrumentId = graph->addNode(node);
            instrumentNodes_[track.id] = node;
            graph->connect(instrumentId, 0, input, 0);
        }
        const NodeId pre = addPluginChain(input, track.plugins);
        addGain(track, pre);
        channels[track.id].input = input;
    }

    for (const auto& bus : edit.buses()) {
        const NodeId input = graph->addNode(std::make_shared<SummingNode>(1));
        const NodeId pre = addPluginChain(input, bus.plugins);
        const NodeId post = addGain(bus, pre);
        channels[bus.id].input = input;
        channels[bus.id].postFader = post;
        if (bus.id == document::kMainBusId) master_ = trackGains_[bus.id];
    }

    auto connectTarget = [&](const std::string& ownerId, NodeId source,
                             document::RouteTarget target) {
        if (anySoloed && !soloEdges.count(edgeKey(ownerId, targetKey(target)))) {
            return;
        }
        if (target.kind == document::RouteTarget::Kind::AudioTrack ||
            target.kind == document::RouteTarget::Kind::Bus) {
            auto destination = channels.find(target.targetId);
            if (destination != channels.end()) {
                graph->connect(source, 0, destination->second.input, 0);
            }
            return;
        }
        if (target.kind != document::RouteTarget::Kind::HardwareOutput) return;

        // A fresh one-output session adapts Main's default stereo assignment
        // to Output 1 without rewriting the saved project route.
        if (ownerId == document::kMainBusId && physicalChannels == 1 &&
            target.hardware.firstChannel == 0 && target.hardware.channelCount == 2) {
            target.hardware.channelCount = 1;
        }
        const int first = target.hardware.firstChannel;
        const int count = target.hardware.channelCount;
        if (first < 0 || first >= physicalChannels ||
            (count == 2 && first + 1 >= physicalChannels)) return;
        const int pair = first / 2;
        const NodeId mapper = graph->addNode(
            std::make_shared<HardwareRouteNode>(first % 2, count));
        graph->connect(source, 0, mapper, 0);
        graph->connect(mapper, 0, hardwareRootId, pair);
    };

    auto routeChannel = [&](const auto& channel) {
        const auto found = channels.find(channel.id);
        if (found == channels.end()) return;
        connectTarget(channel.id, found->second.postFader, channel.mainOutput);
        for (const auto& send : channel.sends) {
            const NodeId tap = send.tap == document::SendTap::PreFader
                ? found->second.preFader : found->second.postFader;
            const double sendGain = send.muted ? 0.0 : send.gain;
            const NodeId level = graph->addNode(
                std::make_shared<SendLevelNode>(sendGain));
            graph->connect(tap, 0, level, 0);
            connectTarget(channel.id, level, send.target);
        }
    };
    for (const auto& track : edit.tracks()) routeChannel(track);
    for (const auto& track : edit.midiTracks()) routeChannel(track);
    for (const auto& bus : edit.buses()) routeChannel(bus);

    std::unordered_set<std::string> liveSlots;
    for (const auto& track : edit.tracks()) {
        for (const auto& slot : track.plugins) liveSlots.insert(slot.id);
    }
    for (const auto& track : edit.midiTracks()) {
        if (!track.instrument.id.empty()) liveSlots.insert(track.instrument.id);
        for (const auto& slot : track.plugins) liveSlots.insert(slot.id);
    }
    for (const auto& bus : edit.buses()) {
        for (const auto& slot : bus.plugins) liveSlots.insert(slot.id);
    }
    for (auto it = pluginInstances_.begin(); it != pluginInstances_.end();) {
        if (!liveSlots.count(it->first)) {
            if (it->second) it->second->unload();
            pluginLatencies_.erase(it->first);
            it = pluginInstances_.erase(it);
        } else {
            ++it;
        }
    }
    return graph;
}

GraphBuilder::~GraphBuilder() = default;

} // namespace dave::engine
