#include "engine/GraphBuilder.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <cstdio>

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
        auto gainId = graph->addNode(gain);
        trackGains_[track.id] = gain;

        if (track.clips.empty()) {
            graph->connect(gainId, 0, masterSumId, trackIndex);
            ++trackIndex;
            continue;
        }

        auto trackSum = std::make_shared<SummingNode>(static_cast<int>(track.clips.size()));
        auto trackSumId = graph->addNode(trackSum);

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
            graph->connect(nodeId, 0, trackSumId, clipIdx);
            ++clipIdx;
        }

        graph->connect(trackSumId, 0, gainId, 0);
        graph->connect(gainId, 0, masterSumId, trackIndex);
        ++trackIndex;
    }

    return graph;
}

} // namespace dave::engine
