#include "document/Edit.h"
#include "document/Sha256.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <algorithm>

#include <cstdio>

namespace dave::document {

std::string Edit::newId(const char* prefix) const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%llu", prefix,
                  static_cast<unsigned long long>(idCounter_ + 1));
    return buf;
}

AssetId Edit::importAsset(const std::string& filePath) {
    std::string hash = sha256HexOfFile(filePath);
    if (hash.empty()) {
        std::fprintf(stderr, "Dave: importAsset: failed to hash %s\n", filePath.c_str());
        return {};
    }
    AssetId id{hash};
    if (assets_.count(id)) {
        return id; // already imported (dedupe by content)
    }

    // Probe the WAV header for format info. dr_wav gives us sample rate,
    // channels, and total frames without decoding the whole file.
    drwav wav;
    if (!drwav_init_file(&wav, filePath.c_str(), nullptr)) {
        std::fprintf(stderr, "Dave: importAsset: not a WAV: %s\n", filePath.c_str());
        return {};
    }
    AudioAsset asset;
    asset.id = id;
    asset.path = filePath;
    asset.sampleRate = static_cast<int>(wav.sampleRate);
    asset.channels = static_cast<int>(wav.channels);
    asset.lengthSamples = static_cast<int64_t>(wav.totalPCMFrameCount);
    drwav_uninit(&wav);

    assets_.emplace(id, std::move(asset));
    notifyChanged();
    return id;
}

const AudioAsset* Edit::asset(const AssetId& id) const {
    auto it = assets_.find(id);
    return it == assets_.end() ? nullptr : &it->second;
}

std::string Edit::addTrack(const std::string& name) {
    Track t;
    t.id = newId("track_");
    ++idCounter_;
    t.name = name.empty() ? "Track " + std::to_string(idCounter_) : name;
    tracks_.push_back(std::move(t));
    notifyChanged();
    return tracks_.back().id;
}

Track* Edit::track(const std::string& id) {
    for (auto& t : tracks_) if (t.id == id) return &t;
    return nullptr;
}

const Track* Edit::track(const std::string& id) const {
    for (const auto& t : tracks_) if (t.id == id) return &t;
    return nullptr;
}

bool Edit::removeTrack(const std::string& id) {
    for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
        if (it->id == id) {
            tracks_.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

std::string Edit::addClip(const std::string& trackId, AudioClip clip) {
    Track* t = track(trackId);
    if (t == nullptr) {
        return "";
    }
    clip.id = newId("clip_");
    ++idCounter_;
    t->clips.push_back(std::move(clip));
    notifyChanged();
    return t->clips.back().id;
}

AudioClip* Edit::clip(const std::string& trackId, const std::string& clipId) {
    Track* t = track(trackId);
    if (t == nullptr) return nullptr;
    for (auto& c : t->clips) if (c.id == clipId) return &c;
    return nullptr;
}

bool Edit::removeClip(const std::string& trackId, const std::string& clipId) {
    Track* t = track(trackId);
    if (t == nullptr) return false;
    for (auto it = t->clips.begin(); it != t->clips.end(); ++it) {
        if (it->id == clipId) {
            t->clips.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

std::string Edit::addPlugin(const std::string& trackId, PluginSlot slot) {
    Track* t = track(trackId);
    if (t == nullptr) return "";
    slot.id = newId("plugin_");
    ++idCounter_;
    t->plugins.push_back(std::move(slot));
    notifyChanged();
    return t->plugins.back().id;
}

bool Edit::removePlugin(const std::string& trackId, const std::string& slotId) {
    Track* t = track(trackId);
    if (t == nullptr) return false;
    for (auto it = t->plugins.begin(); it != t->plugins.end(); ++it) {
        if (it->id == slotId) {
            t->plugins.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

// ─── Marker tracks ──────────────────────────────────────────────────────────

std::string Edit::addMarkerTrack(const std::string& name) {
    MarkerTrack mt;
    mt.id = newId("mtrack_");
    ++idCounter_;
    mt.name = name.empty() ? ("Markers " + std::to_string(idCounter_)) : name;
    markerTracks_.push_back(std::move(mt));
    notifyChanged();
    return markerTracks_.back().id;
}

bool Edit::removeMarkerTrack(const std::string& trackId) {
    for (auto it = markerTracks_.begin(); it != markerTracks_.end(); ++it) {
        if (it->id == trackId) {
            markerTracks_.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

MarkerTrack* Edit::markerTrack(const std::string& trackId) {
    for (auto& mt : markerTracks_) if (mt.id == trackId) return &mt;
    return nullptr;
}

// ─── Markers ────────────────────────────────────────────────────────────────

std::string Edit::addMarker(const std::string& trackId, Marker marker) {
    MarkerTrack* mt = markerTrack(trackId);
    if (mt == nullptr) return "";
    marker.id = newId("marker_");
    ++idCounter_;
    mt->markers.push_back(std::move(marker));
    notifyChanged();
    return mt->markers.back().id;
}

Marker* Edit::marker(const std::string& trackId, const std::string& markerId) {
    MarkerTrack* mt = markerTrack(trackId);
    if (mt == nullptr) return nullptr;
    for (auto& m : mt->markers) if (m.id == markerId) return &m;
    return nullptr;
}

bool Edit::removeMarker(const std::string& trackId, const std::string& markerId) {
    MarkerTrack* mt = markerTrack(trackId);
    if (mt == nullptr) return false;
    for (auto it = mt->markers.begin(); it != mt->markers.end(); ++it) {
        if (it->id == markerId) {
            mt->markers.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

const Marker* Edit::activeLoopMarker() const {
    for (const auto& mt : markerTracks_) {
        for (const auto& m : mt.markers) {
            if (m.kind == MarkerKind::Loop && m.length > 0 &&
                m.posMode == MarkerPosMode::Sample) {
                return &m;
            }
        }
    }
    return nullptr;
}

// ─── Video tracks ───────────────────────────────────────────────────────────

std::string Edit::addVideoTrack(const std::string& name) {
    VideoTrack vt;
    vt.id = newId("vtrack_");
    ++idCounter_;
    vt.name = name.empty() ? ("Video " + std::to_string(idCounter_)) : name;
    videoTracks_.push_back(std::move(vt));
    notifyChanged();
    return videoTracks_.back().id;
}

std::string Edit::addVideoClip(const std::string& trackId, VideoClip clip) {
    for (auto& vt : videoTracks_) {
        if (vt.id == trackId) {
            clip.id = newId("vclip_");
            ++idCounter_;
            // Default length = full source duration if not set.
            if (clip.length <= 0 && clip.fps > 0.0) {
                clip.length = static_cast<int64_t>(clip.durationSeconds * 48000.0);
            }
            vt.clips.push_back(std::move(clip));
            notifyChanged();
            return vt.clips.back().id;
        }
    }
    return "";
}

bool Edit::removeVideoClip(const std::string& trackId, const std::string& clipId) {
    for (auto& vt : videoTracks_) {
        if (vt.id != trackId) continue;
        for (auto it = vt.clips.begin(); it != vt.clips.end(); ++it) {
            if (it->id == clipId) {
                vt.clips.erase(it);
                notifyChanged();
                return true;
            }
        }
    }
    return false;
}

const VideoClip* Edit::videoClipAt(int64_t timelineSample) const {
    for (const auto& vt : videoTracks_) {
        if (!vt.visible) continue;
        for (const auto& c : vt.clips) {
            int64_t len = (c.length > 0) ? c.length
                : static_cast<int64_t>(c.durationSeconds * 48000.0);
            if (timelineSample >= c.timelineStart &&
                timelineSample < c.timelineStart + len) {
                return &c;
            }
        }
    }
    return nullptr;
}

int64_t Edit::contentEndSamples() const {
    int64_t end = 0;
    for (const auto& t : tracks_) {
        for (const auto& c : t.clips) {
            end = std::max(end, c.timelineStart + c.length);
        }
    }
    for (const auto& vt : videoTracks_) {
        for (const auto& c : vt.clips) {
            int64_t len = (c.length > 0) ? c.length
                : static_cast<int64_t>(c.durationSeconds * 48000.0);
            end = std::max(end, c.timelineStart + len);
        }
    }
    // Add a small tail (0.5s) so the last frame/sample isn't cut.
    return end + 24000;
}

} // namespace dave::document
