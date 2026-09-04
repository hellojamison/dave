// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/AudioImportPolicy.h"
#include "document/Edit.h"
#include "document/Sha256.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <algorithm>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace dave::document {

namespace {

RouteTarget* mainOutputFor(Edit& edit, const std::string& id) {
    if (auto* track = edit.track(id)) return &track->mainOutput;
    return nullptr;
}

std::vector<AuxSend>* sendsFor(Edit& edit, const std::string& id) {
    if (auto* track = edit.track(id)) return &track->sends;
    return nullptr;
}

const std::vector<AuxSend>* sendsFor(const Edit& edit, const std::string& id) {
    if (const auto* track = edit.track(id)) return &track->sends;
    return nullptr;
}

bool validHardwareSpan(const HardwareChannelSpan& span, bool fixedOutputPair) {
    if (span.firstChannel < 0 || (span.channelCount != 1 && span.channelCount != 2)) {
        return false;
    }
    // Stereo hardware destinations are the fixed pairs 1-2, 3-4, ... .
    return !fixedOutputPair || span.channelCount != 2 ||
           (span.firstChannel % 2) == 0;
}

} // namespace

Edit::Edit() {
    ensureMainBus_();
}

void Edit::ensureMainBus_() {
    if (track(kMainBusId) != nullptr) return;
    BusTrack main;
    main.id = kMainBusId;
    main.name = "Main";
    main.isMain = true;
    main.mainOutput = RouteTarget::hardwareOutput(0, 2);
    tracks_.push_back(std::move(main));
    normalizeChain_(tracks_.back());
}

std::string Edit::newId(const char* prefix) const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%llu", prefix,
                  static_cast<unsigned long long>(idCounter_ + 1));
    return buf;
}

AssetId Edit::importAsset(const std::string& filePath) {
    // Reject before hashing: hashing a sparse or genuinely huge file would
    // still read every byte even though the in-memory decoder cannot open it.
    std::error_code sizeError;
    const auto encodedBytes = std::filesystem::file_size(filePath, sizeError);
    if (!sizeError && !audio::canDecodeFileInMemory(encodedBytes)) {
        std::fprintf(stderr,
                     "Dave: WAV import refused: files larger than 4 GiB "
                     "require streaming playback: %s\n",
                     filePath.c_str());
        return {};
    }

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

// Main is the last row, always. Every creator inserts ahead of it, which is
// the one ordering rule the single track list has.
std::vector<Track>::iterator Edit::mainRow_() {
    return std::find_if(tracks_.begin(), tracks_.end(),
                        [](const Track& t) { return t.isMain; });
}

std::string Edit::addTrack(const std::string& name) {
    Track t;
    t.id = newId("track_");
    ++idCounter_;
    t.name = name.empty() ? "Track " + std::to_string(idCounter_) : name;
    const std::string id = t.id;
    tracks_.insert(mainRow_(), std::move(t));
    // Every track has a chain from the moment it exists — a meter and a fader
    // at minimum — so nothing downstream has to handle an empty one.
    normalizeChainFor_(id);
    notifyChanged();
    return id;
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
    // Main is permanent. Anything a route still points at stays too — this
    // guard used to cover audio tracks and buses but not MIDI tracks, so
    // deleting a MIDI track a send referenced left a dangling route.
    if (id == kMainBusId) return false;
    if (routeReferences(RouteTarget::Kind::AudioTrack, id) ||
        routeReferences(RouteTarget::Kind::Bus, id)) {
        return false;
    }
    for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
        if (it->id == id) {
            if (it->isMain) return false;
            tracks_.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

bool Edit::setTrackColor(const std::string& ownerId, std::string color) {
    if (!validTrackColor(color)) return false;
    std::string* destination = nullptr;
    if (auto* value = track(ownerId)) destination = &value->color;
    if (destination == nullptr) return false;
    if (*destination == color) return true;
    *destination = std::move(color);
    notifyChanged();
    return true;
}

std::vector<VolumeAutomationPoint>* Edit::volumeAutomation(
    const std::string& ownerId) {
    if (auto* value = track(ownerId)) return &value->volumeAutomation;
    return nullptr;
}

const std::vector<VolumeAutomationPoint>* Edit::volumeAutomation(
    const std::string& ownerId) const {
    if (const auto* value = track(ownerId)) return &value->volumeAutomation;
    return nullptr;
}

std::string Edit::addVolumeAutomationPoint(
    const std::string& ownerId, int64_t sample, double db) {
    auto* points = volumeAutomation(ownerId);
    if (points == nullptr) return {};
    sample = std::max<int64_t>(0, sample);
    if (std::any_of(points->begin(), points->end(),
                    [sample](const auto& point) {
                        return point.sample == sample;
                    })) {
        return {};
    }
    VolumeAutomationPoint point;
    do {
        point.id = newId("automation_");
        ++idCounter_;
    } while (std::any_of(points->begin(), points->end(),
                         [&](const auto& existing) {
                             return existing.id == point.id;
                         }));
    point.sample = sample;
    point.db = clampVolumeAutomationDb(db);
    points->push_back(point);
    std::sort(points->begin(), points->end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    notifyChanged();
    return point.id;
}

bool Edit::restoreVolumeAutomationPoint_(
    const std::string& ownerId, VolumeAutomationPoint point, size_t index) {
    auto* points = volumeAutomation(ownerId);
    if (points == nullptr || point.id.empty()) return false;
    point.sample = std::max<int64_t>(0, point.sample);
    point.db = clampVolumeAutomationDb(point.db);
    if (std::any_of(points->begin(), points->end(), [&](const auto& existing) {
            return existing.id == point.id || existing.sample == point.sample;
        })) {
        return false;
    }
    points->insert(points->begin() +
                       static_cast<ptrdiff_t>(std::min(index, points->size())),
                   std::move(point));
    std::sort(points->begin(), points->end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    notifyChanged();
    return true;
}

bool Edit::updateVolumeAutomationPoint(
    const std::string& ownerId, VolumeAutomationPoint point) {
    auto* points = volumeAutomation(ownerId);
    if (points == nullptr || point.id.empty()) return false;
    point.sample = std::max<int64_t>(0, point.sample);
    point.db = clampVolumeAutomationDb(point.db);
    if (std::any_of(points->begin(), points->end(), [&](const auto& existing) {
            return existing.id != point.id && existing.sample == point.sample;
        })) {
        return false;
    }
    const auto found = std::find_if(
        points->begin(), points->end(), [&](const auto& existing) {
            return existing.id == point.id;
        });
    if (found == points->end()) return false;
    *found = std::move(point);
    std::sort(points->begin(), points->end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    notifyChanged();
    return true;
}

bool Edit::removeVolumeAutomationPoint(
    const std::string& ownerId, const std::string& pointId) {
    auto* points = volumeAutomation(ownerId);
    if (points == nullptr) return false;
    const auto found = std::find_if(
        points->begin(), points->end(), [&](const auto& point) {
            return point.id == pointId;
        });
    if (found == points->end()) return false;
    points->erase(found);
    notifyChanged();
    return true;
}

bool Edit::replaceVolumeAutomation(
    const std::string& ownerId,
    std::vector<VolumeAutomationPoint> points) {
    auto* destination = volumeAutomation(ownerId);
    if (destination == nullptr) return false;

    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.sample < b.sample;
    });
    std::unordered_set<std::string> ids;
    int64_t previousSample = -1;
    for (auto& point : points) {
        point.sample = std::max<int64_t>(0, point.sample);
        point.db = clampVolumeAutomationDb(point.db);
        if (point.sample == previousSample ||
            (!point.id.empty() && !ids.insert(point.id).second)) {
            return false;
        }
        previousSample = point.sample;
    }
    for (auto& point : points) {
        if (!point.id.empty()) continue;
        do {
            point.id = newId("automation_");
            ++idCounter_;
        } while (!ids.insert(point.id).second);
    }
    if (*destination == points) return true;
    *destination = std::move(points);
    notifyChanged();
    return true;
}

std::vector<PanAutomationPoint>* Edit::panAutomation(
    const std::string& ownerId) {
    if (auto* value = track(ownerId)) return &value->panAutomation;
    return nullptr;
}

const std::vector<PanAutomationPoint>* Edit::panAutomation(
    const std::string& ownerId) const {
    if (const auto* value = track(ownerId)) return &value->panAutomation;
    return nullptr;
}

std::string Edit::addPanAutomationPoint(
    const std::string& ownerId, int64_t sample, double pan) {
    auto* points = panAutomation(ownerId);
    if (points == nullptr) return {};
    sample = std::max<int64_t>(0, sample);
    if (std::any_of(points->begin(), points->end(),
                    [sample](const auto& point) {
                        return point.sample == sample;
                    })) {
        return {};
    }
    PanAutomationPoint point;
    do {
        point.id = newId("pan_automation_");
        ++idCounter_;
    } while (std::any_of(points->begin(), points->end(),
                         [&](const auto& existing) {
                             return existing.id == point.id;
                         }));
    point.sample = sample;
    point.pan = clampPanAutomation(pan);
    points->push_back(point);
    std::sort(points->begin(), points->end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    notifyChanged();
    return point.id;
}

bool Edit::restorePanAutomationPoint_(
    const std::string& ownerId, PanAutomationPoint point, size_t index) {
    auto* points = panAutomation(ownerId);
    if (points == nullptr || point.id.empty()) return false;
    point.sample = std::max<int64_t>(0, point.sample);
    point.pan = clampPanAutomation(point.pan);
    if (std::any_of(points->begin(), points->end(), [&](const auto& existing) {
            return existing.id == point.id || existing.sample == point.sample;
        })) {
        return false;
    }
    points->insert(points->begin() +
                       static_cast<ptrdiff_t>(std::min(index, points->size())),
                   std::move(point));
    std::sort(points->begin(), points->end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    notifyChanged();
    return true;
}

bool Edit::updatePanAutomationPoint(
    const std::string& ownerId, PanAutomationPoint point) {
    auto* points = panAutomation(ownerId);
    if (points == nullptr || point.id.empty()) return false;
    point.sample = std::max<int64_t>(0, point.sample);
    point.pan = clampPanAutomation(point.pan);
    if (std::any_of(points->begin(), points->end(), [&](const auto& existing) {
            return existing.id != point.id && existing.sample == point.sample;
        })) {
        return false;
    }
    const auto found = std::find_if(
        points->begin(), points->end(), [&](const auto& existing) {
            return existing.id == point.id;
        });
    if (found == points->end()) return false;
    *found = std::move(point);
    std::sort(points->begin(), points->end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    notifyChanged();
    return true;
}

bool Edit::removePanAutomationPoint(
    const std::string& ownerId, const std::string& pointId) {
    auto* points = panAutomation(ownerId);
    if (points == nullptr) return false;
    const auto found = std::find_if(
        points->begin(), points->end(), [&](const auto& point) {
            return point.id == pointId;
        });
    if (found == points->end()) return false;
    points->erase(found);
    notifyChanged();
    return true;
}

bool Edit::replacePanAutomation(
    const std::string& ownerId,
    std::vector<PanAutomationPoint> points) {
    auto* destination = panAutomation(ownerId);
    if (destination == nullptr) return false;

    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.sample < b.sample;
    });
    std::unordered_set<std::string> ids;
    int64_t previousSample = -1;
    for (auto& point : points) {
        point.sample = std::max<int64_t>(0, point.sample);
        point.pan = clampPanAutomation(point.pan);
        if (point.sample == previousSample ||
            (!point.id.empty() && !ids.insert(point.id).second)) {
            return false;
        }
        previousSample = point.sample;
    }
    for (auto& point : points) {
        if (!point.id.empty()) continue;
        do {
            point.id = newId("pan_automation_");
            ++idCounter_;
        } while (!ids.insert(point.id).second);
    }
    if (*destination == points) return true;
    *destination = std::move(points);
    notifyChanged();
    return true;
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

bool Edit::restoreClip_(const std::string& trackId, AudioClip clip) {
    Track* t = track(trackId);
    if (t == nullptr || clip.id.empty()) return false;
    const auto duplicate = std::find_if(
        t->clips.begin(), t->clips.end(),
        [&](const AudioClip& existing) { return existing.id == clip.id; });
    if (duplicate != t->clips.end()) return false;
    t->clips.push_back(std::move(clip));
    notifyChanged();
    return true;
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
            pruneClipGroups_();
            notifyChanged();
            return true;
        }
    }
    return false;
}

std::string Edit::addPlugin(const std::string& trackId, PluginSlot slot) {
    std::vector<PluginSlot>* plugins = nullptr;
    if (Track* t = track(trackId)) plugins = &t->plugins;
    if (plugins == nullptr) return "";
    slot.id = newId("plugin_");
    ++idCounter_;
    plugins->push_back(std::move(slot));
    // The chain is repaired here rather than by callers: every mutation that
    // can disturb it goes through one of these functions, and one that forgot
    // would leave an insert with no position in the signal path.
    normalizeChainFor_(trackId);
    notifyChanged();
    return plugins->back().id;
}

const PluginSlot* Edit::pluginSlot(const std::string& slotId) const {
    if (slotId.empty()) return nullptr;
    for (const auto& track : tracks_) {
        if (track.instrument.id == slotId) return &track.instrument;
        for (const auto& slot : track.plugins) {
            if (slot.id == slotId) return &slot;
        }
    }
    return nullptr;
}

bool Edit::setPluginBypass(const std::string& slotId, bool bypass) {
    const auto* found = pluginSlot(slotId);
    if (found == nullptr) return false;
    auto* mutableSlot = const_cast<PluginSlot*>(found);
    if (mutableSlot->bypass == bypass) return false;
    mutableSlot->bypass = bypass;
    notifyChanged();
    return true;
}

bool Edit::removePlugin(const std::string& trackId, const std::string& slotId) {
    std::vector<PluginSlot>* plugins = nullptr;
    if (Track* t = track(trackId)) plugins = &t->plugins;
    if (plugins == nullptr) return false;
    for (auto it = plugins->begin(); it != plugins->end(); ++it) {
        if (it->id == slotId) {
            plugins->erase(it);
            normalizeChainFor_(trackId);
            notifyChanged();
            return true;
        }
    }
    return false;
}

// ─── MIDI tracks ────────────────────────────────────────────────────────────

std::string Edit::addMidiTrack(const std::string& name) {
    // Same object as any other track; only the id prefix and default name say
    // what it was made for. Buses stay last, so a new row lands before Main.
    Track mt;
    mt.id = newId("miditrack_");
    ++idCounter_;
    mt.name = name.empty() ? "MIDI " + std::to_string(idCounter_) : name;
    const std::string id = mt.id;
    tracks_.insert(mainRow_(), std::move(mt));
    notifyChanged();
    return id;
}

std::string Edit::addBus(const std::string& name) {
    Track candidate;
    do {
        candidate.id = newId("bus_");
        ++idCounter_;
    } while (track(candidate.id));
    candidate.name = name.empty() ? "Bus " + std::to_string(idCounter_) : name;
    candidate.mainOutput = RouteTarget::bus();
    const std::string id = candidate.id;
    tracks_.insert(mainRow_(), std::move(candidate));
    notifyChanged();
    return id;
}

bool Edit::restoreTrack_(Track restored, size_t index) {
    if (restored.id.empty() || restored.isMain) return false;
    if (track(restored.id)) return false;
    // Main is pinned last, so a restore can never land after it.
    size_t mainIndex = tracks_.size();
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].isMain) { mainIndex = i; break; }
    }
    index = std::min(index, mainIndex);
    tracks_.insert(tracks_.begin() + static_cast<ptrdiff_t>(index),
                   std::move(restored));
    const auto validation = validateRouting();
    if (!validation.ok) {
        tracks_.erase(tracks_.begin() + static_cast<ptrdiff_t>(index));
        return false;
    }
    normalizeChain_(tracks_[index]);
    notifyChanged();
    return true;
}

bool Edit::routeReferences(RouteTarget::Kind kind, const std::string& id) const {
    const auto references = [&](const RouteTarget& target) {
        return target.kind == kind && target.targetId == id;
    };
    const auto channelReferences = [&](const auto& channel) {
        if (references(channel.mainOutput)) return true;
        if (std::any_of(channel.extraOutputs.begin(), channel.extraOutputs.end(),
                        references)) {
            return true;
        }
        return std::any_of(channel.sends.begin(), channel.sends.end(),
                           [&](const AuxSend& send) { return references(send.target); });
    };
    for (const auto& track : tracks_) if (channelReferences(track)) return true;
    return false;
}

Edit::RoutingValidation Edit::validateRouting() const {
    std::unordered_set<std::string> audioIds;
    std::unordered_set<std::string> busIds;
    std::unordered_set<std::string> allOwnerIds;
    std::vector<std::string> owners;
    for (const auto& track : tracks_) {
        if (track.id.empty() || !audioIds.insert(track.id).second) {
            return {false, "track has an empty or duplicate id"};
        }
        // One track list, so a route naming this channel resolves whether it
        // was stored as an AudioTrack target or a Bus target. Older documents
        // contain both spellings for what is now the same thing.
        busIds.insert(track.id);
        if (!allOwnerIds.insert(track.id).second) {
            return {false, "routing owner ids must be globally unique"};
        }
        owners.push_back(track.id);
        if (!validHardwareSpan(track.hardwareInput, false)) {
            return {false, "audio track has an invalid hardware input span"};
        }
    }
    const auto* main = mainBus();
    if (!main || !main->isMain) return {false, "permanent Main bus is missing"};

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    const auto checkTarget = [&](const std::string& ownerId,
                                 const RouteTarget& target,
                                 bool ownerIsMain) -> RoutingValidation {
        if (ownerIsMain && target.kind != RouteTarget::Kind::HardwareOutput) {
            return {false, "Main may route or send only to hardware"};
        }
        switch (target.kind) {
            case RouteTarget::Kind::None:
                return {true, {}};
            case RouteTarget::Kind::HardwareOutput:
                return validHardwareSpan(target.hardware, true)
                    ? RoutingValidation{}
                    : RoutingValidation{false, "invalid hardware output span"};
            case RouteTarget::Kind::AudioTrack:
                if (!audioIds.count(target.targetId)) {
                    return {false, "route references a missing audio track"};
                }
                if (target.targetId == ownerId) return {false, "self-route refused"};
                adjacency[ownerId].push_back(target.targetId);
                return {};
            case RouteTarget::Kind::Bus:
                if (!busIds.count(target.targetId)) {
                    return {false, "route references a missing bus"};
                }
                if (target.targetId == ownerId) return {false, "self-route refused"};
                adjacency[ownerId].push_back(target.targetId);
                return {};
        }
        return {false, "unknown route target"};
    };

    const auto checkChannel = [&](const auto& channel) -> RoutingValidation {
        const bool isMain = channel.id == kMainBusId;
        auto result = checkTarget(channel.id, channel.mainOutput, isMain);
        if (!result.ok) return result;
        for (const auto& extra : channel.extraOutputs) {
            if (extra.kind == RouteTarget::Kind::None) {
                return {false, "an additional output must go somewhere"};
            }
            result = checkTarget(channel.id, extra, isMain);
            if (!result.ok) return result;
        }
        std::unordered_set<std::string> sendIds;
        for (const auto& send : channel.sends) {
            if (send.id.empty() || !sendIds.insert(send.id).second) {
                return {false, "send has an empty or duplicate id"};
            }
            if (send.gain < 0.0) return {false, "send gain must be non-negative"};
            result = checkTarget(channel.id, send.target, isMain);
            if (!result.ok) return result;
        }
        return {};
    };
    for (const auto& track : tracks_) {
        auto result = checkChannel(track); if (!result.ok) return result;
    }

    enum class Mark { Unseen, Visiting, Done };
    std::unordered_map<std::string, Mark> marks;
    std::function<bool(const std::string&)> visitsCycle = [&](const std::string& id) {
        if (marks[id] == Mark::Visiting) return true;
        if (marks[id] == Mark::Done) return false;
        marks[id] = Mark::Visiting;
        for (const auto& next : adjacency[id]) if (visitsCycle(next)) return true;
        marks[id] = Mark::Done;
        return false;
    };
    for (const auto& owner : owners) {
        if (visitsCycle(owner)) return {false, "routing cycle refused"};
    }
    return {};
}

Edit::RoutingValidation Edit::validateMainOutput(
    const std::string& ownerId, const RouteTarget& target) const {
    Edit candidate = *this;
    candidate.setChangeListener({});
    RouteTarget* output = mainOutputFor(candidate, ownerId);
    if (!output) return {false, "route owner does not exist"};
    *output = target;
    return candidate.validateRouting();
}

Edit::RoutingValidation Edit::validateSendTarget(
    const std::string& ownerId, const RouteTarget& target) const {
    Edit candidate = *this;
    candidate.setChangeListener({});
    auto* list = sendsFor(candidate, ownerId);
    if (!list) return {false, "route owner does not exist"};
    AuxSend send;
    send.id = "__validation_send__";
    send.target = target;
    list->push_back(std::move(send));
    return candidate.validateRouting();
}

bool Edit::addOutput(const std::string& ownerId, RouteTarget target) {
    Track* owner = track(ownerId);
    if (owner == nullptr) return false;
    // The same destination twice would just double the signal into it.
    if (owner->mainOutput == target) return false;
    for (const auto& extra : owner->extraOutputs) {
        if (extra == target) return false;
    }
    owner->extraOutputs.push_back(std::move(target));
    if (!validateRouting().ok) {
        owner->extraOutputs.pop_back();
        return false;
    }
    notifyChanged();
    return true;
}

bool Edit::removeOutput(const std::string& ownerId, const RouteTarget& target) {
    Track* owner = track(ownerId);
    if (owner == nullptr) return false;
    auto& extras = owner->extraOutputs;
    const auto it = std::find(extras.begin(), extras.end(), target);
    if (it == extras.end()) return false;
    extras.erase(it);
    notifyChanged();
    return true;
}

bool Edit::setMainOutput(const std::string& ownerId, RouteTarget target) {
    RouteTarget* output = mainOutputFor(*this, ownerId);
    if (!output || *output == target) return false;
    const RouteTarget previous = *output;
    *output = std::move(target);
    if (!validateRouting().ok) {
        *output = previous;
        return false;
    }
    notifyChanged();
    return true;
}

bool Edit::setTrackHardwareInput(const std::string& trackId,
                                 HardwareChannelSpan span) {
    auto* candidate = track(trackId);
    if (!candidate || !validHardwareSpan(span, false) ||
        candidate->hardwareInput == span) {
        return false;
    }
    candidate->hardwareInput = span;
    // Keep the legacy fields synchronized until v1 callers are retired.
    candidate->inputChannel = span.firstChannel;
    candidate->inputChannelCount = span.channelCount;
    notifyChanged();
    return true;
}

bool Edit::setInputMonitor(const std::string& trackId, bool enabled) {
    auto* candidate = track(trackId);
    if (!candidate || candidate->inputMonitor == enabled) return false;
    candidate->inputMonitor = enabled;
    notifyChanged();
    return true;
}

std::string Edit::addSend(const std::string& ownerId, AuxSend send) {
    auto* list = sendsFor(*this, ownerId);
    if (!list) return {};
    const auto sendIdExists = [&](const std::string& id) {
        const auto has = [&](const auto& channel) {
            return std::any_of(channel.sends.begin(), channel.sends.end(),
                               [&](const AuxSend& existing) {
                                   return existing.id == id;
                               });
        };
        for (const auto& track : tracks_) if (has(track)) return true;
        return false;
    };
    do {
        send.id = newId("send_");
        ++idCounter_;
    } while (sendIdExists(send.id));
    list->push_back(std::move(send));
    if (!validateRouting().ok) {
        list->pop_back();
        return {};
    }
    normalizeChainFor_(ownerId);
    notifyChanged();
    return list->back().id;
}

double Edit::tempoBpm() const {
    return tempoMap_.empty() ? 120.0 : tempoMap_.front().bpm;
}

void Edit::setTempoBpm(double bpm) {
    setTempoChange(1, 1, bpm);
}

void Edit::setTempoMap(std::vector<TempoChange> map) {
    auto normalized = normalizeTempoMap(std::move(map));
    if (normalized == tempoMap_) return;
    tempoMap_ = std::move(normalized);
    notifyChanged();
}

bool Edit::setTempoChange(int bar, int beat, double bpm) {
    // Refused rather than repaired here: normalizeTempoMap would silently
    // substitute 120, and a UI that sent 0 would look like it had worked.
    if (bar < 1 || beat < 1 || !(bpm > 0.0) || bpm > 999.0) return false;
    auto map = tempoMap_;
    map.push_back(TempoChange{bar, beat, bpm});
    auto normalized = normalizeTempoMap(std::move(map));
    if (normalized == tempoMap_) return false;
    tempoMap_ = std::move(normalized);
    notifyChanged();
    return true;
}

bool Edit::removeTempoChange(int bar, int beat) {
    // Bar 1 beat 1 is the session's tempo, not a change.
    if (bar <= 1 && beat <= 1) return false;
    auto map = tempoMap_;
    const auto before = map.size();
    map.erase(std::remove_if(map.begin(), map.end(),
                             [&](const TempoChange& c) {
                                 return c.bar == bar && c.beat == beat;
                             }),
              map.end());
    if (map.size() == before) return false;
    tempoMap_ = normalizeTempoMap(std::move(map));
    notifyChanged();
    return true;
}

void Edit::setMeterMap(std::vector<TimeSignature> map) {
    auto normalized = normalizeMeterMap(std::move(map));
    if (normalized == meterMap_) return;
    meterMap_ = std::move(normalized);
    notifyChanged();
}

bool Edit::setTimeSignature(int bar, int numerator, int denominator) {
    if (bar < 1 || numerator < 1) return false;
    auto map = meterMap_;
    map.push_back(TimeSignature{bar, numerator, denominator});
    // normalizeMeterMap keeps the last entry for a bar, so this replaces an
    // existing change rather than sitting beside it.
    auto normalized = normalizeMeterMap(std::move(map));
    if (normalized == meterMap_) return false;
    meterMap_ = std::move(normalized);
    notifyChanged();
    return true;
}

bool Edit::removeTimeSignature(int bar) {
    // Bar 1 is the session's meter, not a change: removing it would leave
    // every bar before the next change with no meter at all.
    if (bar <= 1) return false;
    auto map = meterMap_;
    const auto before = map.size();
    map.erase(std::remove_if(map.begin(), map.end(),
                             [&](const TimeSignature& s) {
                                 return s.bar == bar;
                             }),
              map.end());
    if (map.size() == before) return false;
    meterMap_ = normalizeMeterMap(std::move(map));
    notifyChanged();
    return true;
}

void Edit::loadMusicalTime_(std::vector<TempoChange> tempo,
                            std::vector<TimeSignature> map) {
    tempoMap_ = normalizeTempoMap(std::move(tempo));
    meterMap_ = normalizeMeterMap(std::move(map));
}

// ─── Clip groups ────────────────────────────────────────────────────────────

namespace {

bool memberExists(const Edit& edit, const ClipGroup::Member& member) {
    const auto* track = edit.track(member.trackId);
    if (track == nullptr) return false;
    if (member.midi) {
        return std::any_of(track->midiClips.begin(), track->midiClips.end(),
                           [&](const MidiClip& c) {
                               return c.id == member.clipId;
                           });
    }
    return std::any_of(track->clips.begin(), track->clips.end(),
                       [&](const AudioClip& c) {
                           return c.id == member.clipId;
                       });
}

} // namespace

std::string Edit::addClipGroup(std::vector<ClipGroup::Member> members,
                               int64_t start, int64_t length,
                               std::vector<std::string> trackIds,
                               std::vector<std::string> childGroupIds,
                               std::string name) {
    // Drop anything that does not name a real clip, and anything already in
    // another group — a clip in two groups would move twice for one drag.
    std::vector<ClipGroup::Member> kept;
    kept.reserve(members.size());
    for (auto& member : members) {
        if (!memberExists(*this, member)) continue;
        if (clipGroupContaining(member.trackId, member.clipId) != nullptr) {
            continue;
        }
        const bool duplicate =
            std::any_of(kept.begin(), kept.end(),
                        [&](const ClipGroup::Member& existing) {
                            return existing == member;
                        });
        if (!duplicate) kept.push_back(std::move(member));
    }
    // A group is defined by its range, not by how many clips are in it. An
    // empty one is a legitimate object — a placeholder over a section you are
    // about to fill, which you can drag around like anything else — so the
    // only thing refused is a group with no extent.
    if (length <= 0) return {};

    // Only groups that exist and are not already inside something else. A
    // group with two parents would move twice for one drag, the same way a
    // clip in two groups would.
    childGroupIds.erase(
        std::remove_if(childGroupIds.begin(), childGroupIds.end(),
                       [&](const std::string& id) {
                           return clipGroup(id) == nullptr ||
                                  clipGroupParent(id) != nullptr;
                       }),
        childGroupIds.end());
    // Their rows come with them, so an outer group drawn over a nested one
    // covers everywhere the nested one did.
    for (const auto& childId : childGroupIds) {
        const auto* child = clipGroup(childId);
        if (child == nullptr) continue;
        for (const auto& trackId : child->trackIds) {
            const bool known = std::any_of(trackIds.begin(), trackIds.end(),
                                           [&](const std::string& id) {
                                               return id == trackId;
                                           });
            if (!known) trackIds.push_back(trackId);
        }
    }

    ClipGroup group;
    group.childGroupIds = std::move(childGroupIds);
    // Every member's track counts even if the caller did not list it.
    for (const auto& member : kept) {
        const bool known = std::any_of(trackIds.begin(), trackIds.end(),
                                       [&](const std::string& id) {
                                           return id == member.trackId;
                                       });
        if (!known) trackIds.push_back(member.trackId);
    }
    // A group has to sit somewhere. With no members and no rows named, there
    // is nothing to draw it on.
    if (trackIds.empty()) return {};
    group.trackIds = std::move(trackIds);
    group.timelineStart = std::max<int64_t>(0, start);
    group.length = std::max<int64_t>(0, length);
    group.id = newId("group_");
    ++idCounter_;
    group.name = name.empty() ? "Group " + std::to_string(idCounter_)
                              : std::move(name);
    group.members = std::move(kept);
    const std::string id = group.id;
    clipGroups_.push_back(std::move(group));
    notifyChanged();
    return id;
}

const ClipGroup* Edit::clipGroupParent(const std::string& groupId) const {
    if (groupId.empty()) return nullptr;
    for (const auto& group : clipGroups_) {
        for (const auto& childId : group.childGroupIds) {
            if (childId == groupId) return &group;
        }
    }
    return nullptr;
}

bool Edit::moveClipGroup(const std::string& groupId, int64_t deltaSamples) {
    const auto it = std::find_if(clipGroups_.begin(), clipGroups_.end(),
                                 [&](const ClipGroup& g) {
                                     return g.id == groupId;
                                 });
    if (it == clipGroups_.end() || deltaSamples == 0) return false;

    // Clamped as one object. Clamping each clip against zero separately would
    // squash the members together and lose their spacing, which is the one
    // thing a group exists to keep — and the range would no longer match them.
    const int64_t earliest = earliestInClipGroup_(groupId);
    const int64_t applied = std::max(deltaSamples, -earliest);
    if (applied == 0) return false;

    shiftClipGroup_(groupId, applied);
    notifyChanged();
    return true;
}

// Slide one group and everything it contains, without notifying — the caller
// does that once for the whole move. Recursive, because a nested group's own
// range has to travel with the clips inside it or the box stops sitting over
// its contents.
// The earliest sample anything in this group touches, its nested groups
// included. The clamp uses it so a drag stops the whole tree at zero rather
// than squashing whatever happened to be leftmost.
int64_t Edit::earliestInClipGroup_(const std::string& groupId) const {
    const auto* group = clipGroup(groupId);
    if (group == nullptr) return 0;
    int64_t earliest = group->timelineStart;
    for (const auto& member : group->members) {
        if (member.midi) {
            if (const auto* c = const_cast<Edit*>(this)->midiClip(
                    member.trackId, member.clipId)) {
                earliest = std::min(earliest, c->timelineStart);
            }
        } else if (const auto* c = const_cast<Edit*>(this)->clip(
                       member.trackId, member.clipId)) {
            earliest = std::min(earliest, c->timelineStart);
        }
    }
    for (const auto& childId : group->childGroupIds) {
        earliest = std::min(earliest, earliestInClipGroup_(childId));
    }
    return earliest;
}

void Edit::shiftClipGroup_(const std::string& groupId, int64_t delta) {
    const auto it = std::find_if(clipGroups_.begin(), clipGroups_.end(),
                                 [&](const ClipGroup& g) {
                                     return g.id == groupId;
                                 });
    if (it == clipGroups_.end()) return;
    it->timelineStart += delta;
    const auto members = it->members;
    const auto children = it->childGroupIds;
    for (const auto& member : members) {
        if (member.midi) {
            if (auto* c = midiClip(member.trackId, member.clipId)) {
                c->timelineStart += delta;
            }
        } else if (auto* c = clip(member.trackId, member.clipId)) {
            c->timelineStart += delta;
        }
    }
    for (const auto& childId : children) shiftClipGroup_(childId, delta);
}

int Edit::trackIndexOf_(const std::string& trackId) const {
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == trackId) return static_cast<int>(i);
    }
    return -1;
}

bool Edit::moveClipGroupTracks(const std::string& groupId, int rowDelta) {
    if (rowDelta == 0 || clipGroup(groupId) == nullptr) return false;
    if (!clipGroupTracksFit_(groupId, rowDelta)) return false;
    shiftClipGroupTracks_(groupId, rowDelta);
    notifyChanged();
    return true;
}

// Every track the group touches — its own rows, its members' and its nested
// groups' — has to have a row to land on. Checked before anything moves so a
// refusal leaves the document untouched.
bool Edit::clipGroupTracksFit_(const std::string& groupId,
                               int rowDelta) const {
    const auto* group = clipGroup(groupId);
    if (group == nullptr) return false;
    const int count = static_cast<int>(tracks_.size());
    auto fits = [&](const std::string& trackId) {
        const int at = trackIndexOf_(trackId);
        if (at < 0) return false;
        const int to = at + rowDelta;
        return to >= 0 && to < count;
    };
    for (const auto& trackId : group->trackIds) {
        if (!fits(trackId)) return false;
    }
    for (const auto& member : group->members) {
        if (!fits(member.trackId)) return false;
    }
    for (const auto& childId : group->childGroupIds) {
        if (!clipGroupTracksFit_(childId, rowDelta)) return false;
    }
    return true;
}

void Edit::shiftClipGroupTracks_(const std::string& groupId, int rowDelta) {
    const auto it = std::find_if(clipGroups_.begin(), clipGroups_.end(),
                                 [&](const ClipGroup& g) {
                                     return g.id == groupId;
                                 });
    if (it == clipGroups_.end()) return;
    auto shifted = [&](const std::string& trackId) -> std::string {
        const int at = trackIndexOf_(trackId);
        return tracks_[static_cast<size_t>(at + rowDelta)].id;
    };
    for (auto& trackId : it->trackIds) trackId = shifted(trackId);
    for (auto& member : it->members) {
        const std::string to = shifted(member.trackId);
        auto* from = track(member.trackId);
        auto* dest = track(to);
        if (from == nullptr || dest == nullptr) continue;
        // Lift the clip out of one track's vector and append it to the
        // other's, id and all — a move, not a re-add, so nothing re-mints.
        if (member.midi) {
            auto& v = from->midiClips;
            const auto c = std::find_if(v.begin(), v.end(),
                [&](const MidiClip& m) { return m.id == member.clipId; });
            if (c == v.end()) continue;
            dest->midiClips.push_back(std::move(*c));
            v.erase(c);
        } else {
            auto& v = from->clips;
            const auto c = std::find_if(v.begin(), v.end(),
                [&](const AudioClip& a) { return a.id == member.clipId; });
            if (c == v.end()) continue;
            dest->clips.push_back(std::move(*c));
            v.erase(c);
        }
        member.trackId = to;
    }
    const auto children = it->childGroupIds;
    for (const auto& childId : children) shiftClipGroupTracks_(childId, rowDelta);
}

// ─── Playlists ──────────────────────────────────────────────────────────────

// Give the track its roster if it has none yet: one record for what is
// playing now, named after the track. Idempotent.
void Edit::normalizePlaylists_(Track& track) {
    const bool haveActive = std::any_of(
        track.playlists.begin(), track.playlists.end(),
        [&](const Playlist& p) { return p.id == track.activePlaylistId; });
    if (!track.activePlaylistId.empty() && haveActive) return;
    Playlist active;
    active.id = newId("pl_");
    ++idCounter_;
    active.name = track.name + ".01";
    track.playlists.insert(track.playlists.begin(), std::move(active));
    track.activePlaylistId = track.playlists.front().id;
}

const Playlist* Edit::playlist(const std::string& trackId,
                               const std::string& playlistId) const {
    const Track* t = track(trackId);
    if (t == nullptr) return nullptr;
    for (const auto& p : t->playlists) {
        if (p.id == playlistId) return &p;
    }
    return nullptr;
}

std::vector<Playlist> Edit::playlistRoster(const std::string& trackId) const {
    const Track* t = track(trackId);
    if (t == nullptr) return {};
    std::vector<Playlist> roster;
    for (const auto& p : t->playlists) {
        Playlist entry;
        entry.id = p.id;
        entry.name = p.name;
        roster.push_back(std::move(entry));
    }
    if (roster.empty()) {
        // No roster yet: the one implicit playlist, unnamed until it is
        // materialised. An empty id tells the caller it is the active one.
        Playlist only;
        only.name = t->name + ".01";
        roster.push_back(std::move(only));
    }
    return roster;
}

std::string Edit::addPlaylist(const std::string& trackId, std::string name,
                              bool duplicateActive) {
    Track* t = track(trackId);
    if (t == nullptr) return "";
    normalizePlaylists_(*t);
    Playlist next;
    next.id = newId("pl_");
    ++idCounter_;
    if (name.empty()) {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".%02zu",
                      t->playlists.size() + 1);
        name = t->name + suffix;
    }
    next.name = std::move(name);
    if (duplicateActive) {
        // Copies with their own ids: two playlists must never share a clip
        // id, or a command aimed at one would find the other.
        for (auto clip : t->clips) {
            clip.id = newId("clip_");
            ++idCounter_;
            next.clips.push_back(std::move(clip));
        }
        for (auto clip : t->midiClips) {
            clip.id = newId("mclip_");
            ++idCounter_;
            next.midiClips.push_back(std::move(clip));
        }
    }
    t->playlists.push_back(std::move(next));
    notifyChanged();
    return t->playlists.back().id;
}

bool Edit::switchPlaylist(const std::string& trackId,
                          const std::string& playlistId) {
    Track* t = track(trackId);
    if (t == nullptr) return false;
    normalizePlaylists_(*t);
    if (playlistId == t->activePlaylistId) return false;
    auto find = [&](const std::string& id) {
        return std::find_if(t->playlists.begin(), t->playlists.end(),
                            [&](const Playlist& p) { return p.id == id; });
    };
    const auto next = find(playlistId);
    const auto current = find(t->activePlaylistId);
    if (next == t->playlists.end() || current == t->playlists.end()) {
        return false;
    }
    // Park what is playing, bring the other in. Moves, not copies: the
    // clips keep their ids and their assets either way.
    current->clips = std::move(t->clips);
    current->midiClips = std::move(t->midiClips);
    t->clips = std::move(next->clips);
    t->midiClips = std::move(next->midiClips);
    next->clips.clear();
    next->midiClips.clear();
    t->activePlaylistId = playlistId;
    notifyChanged();
    return true;
}

bool Edit::renamePlaylist(const std::string& trackId,
                          const std::string& playlistId, std::string name) {
    Track* t = track(trackId);
    if (t == nullptr || name.empty()) return false;
    normalizePlaylists_(*t);
    for (auto& p : t->playlists) {
        if (p.id == playlistId) {
            if (p.name == name) return false;
            p.name = std::move(name);
            notifyChanged();
            return true;
        }
    }
    return false;
}

bool Edit::removePlaylist(const std::string& trackId,
                          const std::string& playlistId) {
    Track* t = track(trackId);
    if (t == nullptr || playlistId == t->activePlaylistId) return false;
    const auto it = std::find_if(t->playlists.begin(), t->playlists.end(),
                                 [&](const Playlist& p) {
                                     return p.id == playlistId;
                                 });
    if (it == t->playlists.end()) return false;
    t->playlists.erase(it);
    notifyChanged();
    return true;
}

bool Edit::restorePlaylist_(const std::string& trackId, Playlist playlist,
                            size_t index) {
    Track* t = track(trackId);
    if (t == nullptr || playlist.id.empty()) return false;
    if (this->playlist(trackId, playlist.id) != nullptr) return false;
    const size_t at = std::min(index, t->playlists.size());
    t->playlists.insert(t->playlists.begin() + static_cast<ptrdiff_t>(at),
                        std::move(playlist));
    notifyChanged();
    return true;
}

bool Edit::restoreClipGroup_(ClipGroup group, size_t index) {
    if (group.id.empty()) return false;
    if (clipGroup(group.id) != nullptr) return false;
    const size_t at = std::min(index, clipGroups_.size());
    clipGroups_.insert(clipGroups_.begin() + static_cast<ptrdiff_t>(at),
                       std::move(group));
    notifyChanged();
    return true;
}

bool Edit::removeClipGroup(const std::string& groupId) {
    const auto it = std::find_if(clipGroups_.begin(), clipGroups_.end(),
                                 [&](const ClipGroup& g) {
                                     return g.id == groupId;
                                 });
    if (it == clipGroups_.end()) return false;
    clipGroups_.erase(it);
    // A parent naming a group that has gone would keep pretending to contain
    // it — the child would stay hidden with nothing standing in for it.
    for (auto& group : clipGroups_) {
        group.childGroupIds.erase(
            std::remove(group.childGroupIds.begin(),
                        group.childGroupIds.end(), groupId),
            group.childGroupIds.end());
    }
    notifyChanged();
    return true;
}

const ClipGroup* Edit::clipGroup(const std::string& groupId) const {
    for (const auto& group : clipGroups_) {
        if (group.id == groupId) return &group;
    }
    return nullptr;
}

const ClipGroup* Edit::clipGroupContaining(const std::string& trackId,
                                           const std::string& clipId) const {
    if (clipId.empty()) return nullptr;
    for (const auto& group : clipGroups_) {
        for (const auto& member : group.members) {
            if (member.trackId == trackId && member.clipId == clipId) {
                return &group;
            }
        }
    }
    return nullptr;
}

void Edit::pruneClipGroups_() {
    bool changed = false;
    for (auto& group : clipGroups_) {
        group.childGroupIds.erase(
            std::remove_if(group.childGroupIds.begin(),
                           group.childGroupIds.end(),
                           [&](const std::string& id) {
                               return clipGroup(id) == nullptr;
                           }),
            group.childGroupIds.end());
        const auto before = group.members.size();
        group.members.erase(
            std::remove_if(group.members.begin(), group.members.end(),
                           [&](const ClipGroup::Member& member) {
                               return !memberExists(*this, member);
                           }),
            group.members.end());
        changed = changed || group.members.size() != before;
    }
    // Emptying a group does NOT delete it: the range is the object, and one
    // that vanished when you deleted its last clip would take the section
    // marker with it. Only a group with no extent is dropped, which cannot be
    // created and can only come from a damaged file.
    const auto before = clipGroups_.size();
    clipGroups_.erase(std::remove_if(clipGroups_.begin(), clipGroups_.end(),
                                     [](const ClipGroup& group) {
                                         return group.length <= 0;
                                     }),
                      clipGroups_.end());
    if (changed || clipGroups_.size() != before) notifyChanged();
}

bool Edit::setTrackSoloSafe(const std::string& trackId, bool safe) {
    Track* t = track(trackId);
    if (t == nullptr) return false;
    if (t->soloSafe == safe) return false;
    t->soloSafe = safe;
    notifyChanged();
    return true;
}

bool Edit::setTrackHidden(const std::string& trackId, bool hidden) {
    Track* t = track(trackId);
    if (t == nullptr) return false;
    // Main is where everything ends up; a session with no visible Main has no
    // visible output, and no way to get it back except from a menu that would
    // have to exist only for this case.
    if (t->isMain && hidden) return false;
    if (t->hidden == hidden) return false;
    t->hidden = hidden;
    notifyChanged();
    return true;
}

void Edit::normalizeChain_(Track& track) {
    std::vector<ChainSlot> rebuilt;
    rebuilt.reserve(track.plugins.size() + track.sends.size() + 1);

    const auto stillExists = [&](const ChainSlot& slot) {
        switch (slot.kind) {
            case ChainSlot::Kind::Insert:
                return std::any_of(track.plugins.begin(), track.plugins.end(),
                                   [&](const PluginSlot& p) {
                                       return p.id == slot.id;
                                   });
            case ChainSlot::Kind::Send:
                return std::any_of(track.sends.begin(), track.sends.end(),
                                   [&](const AuxSend& s) {
                                       return s.id == slot.id;
                                   });
            case ChainSlot::Kind::Fader:
                return true;
        }
        return false;
    };

    // Keep the order the user arranged, dropping anything that has gone and
    // collapsing a duplicated Meter or Fader to the first of each.
    bool haveFader = false;
    for (const auto& slot : track.chain) {
        if (!stillExists(slot)) continue;
        if (slot.kind == ChainSlot::Kind::Fader) {
            if (haveFader) continue;
            haveFader = true;
        }
        if (slot.kind == ChainSlot::Kind::Insert ||
            slot.kind == ChainSlot::Kind::Send) {
            const bool already =
                std::any_of(rebuilt.begin(), rebuilt.end(),
                            [&](const ChainSlot& existing) {
                                return existing.kind == slot.kind &&
                                       existing.id == slot.id;
                            });
            if (already) continue;
        }
        rebuilt.push_back(slot);
    }

    const auto insertBeforeFader = [&](ChainSlot slot) {
        const auto fader = std::find_if(
            rebuilt.begin(), rebuilt.end(), [](const ChainSlot& s) {
                return s.kind == ChainSlot::Kind::Fader;
            });
        rebuilt.insert(fader, std::move(slot));
    };
    if (!haveFader) rebuilt.push_back({ChainSlot::Kind::Fader, {}});

    // A new insert goes at the end of the inserts — not merely ahead of the
    // fader, which would drop it past a meter parked there and quietly stop
    // the meter reading the whole chain. If the meter has been dragged
    // somewhere deliberate, this leaves it where it was put.
    const auto appendInsert = [&](ChainSlot slot) {
        auto at = rebuilt.begin();
        bool sawInsert = false;
        for (auto it = rebuilt.begin(); it != rebuilt.end(); ++it) {
            if (it->kind == ChainSlot::Kind::Insert) {
                at = it + 1;
                sawInsert = true;
            }
        }
        if (!sawInsert) at = rebuilt.begin();
        rebuilt.insert(at, std::move(slot));
    };
    for (const auto& plugin : track.plugins) {
        const bool present = std::any_of(
            rebuilt.begin(), rebuilt.end(), [&](const ChainSlot& s) {
                return s.kind == ChainSlot::Kind::Insert && s.id == plugin.id;
            });
        if (!present) appendInsert({ChainSlot::Kind::Insert, plugin.id});
    }
    // Where a NEW send starts. AuxSend::tap is the placement hint and nothing
    // more: once the send is in the chain its position is the truth, which is
    // why the strip moves the row instead of offering a Pre button.
    for (const auto& send : track.sends) {
        const bool present = std::any_of(
            rebuilt.begin(), rebuilt.end(), [&](const ChainSlot& s) {
                return s.kind == ChainSlot::Kind::Send && s.id == send.id;
            });
        if (present) continue;
        if (send.tap == SendTap::PreFader) {
            insertBeforeFader({ChainSlot::Kind::Send, send.id});
        } else {
            rebuilt.push_back({ChainSlot::Kind::Send, send.id});
        }
    }
    track.chain = std::move(rebuilt);
}

void Edit::normalizeChainFor_(const std::string& trackId) {
    if (Track* t = track(trackId)) normalizeChain_(*t);
}

bool Edit::moveChainSlot(const std::string& trackId, size_t from, size_t to) {
    Track* t = track(trackId);
    if (t == nullptr) return false;
    normalizeChain_(*t);
    if (from >= t->chain.size()) return false;
    const size_t target = std::min(to, t->chain.size() - 1);
    if (from == target) return false;
    ChainSlot moved = t->chain[from];
    t->chain.erase(t->chain.begin() + static_cast<ptrdiff_t>(from));
    t->chain.insert(t->chain.begin() + static_cast<ptrdiff_t>(target),
                    std::move(moved));
    notifyChanged();
    return true;
}

bool Edit::moveSend(const std::string& ownerId, const std::string& sendId,
                    size_t newIndex) {
    auto* list = sendsFor(*this, ownerId);
    if (!list || list->empty()) return false;
    const auto it = std::find_if(list->begin(), list->end(),
                                 [&](const AuxSend& s) { return s.id == sendId; });
    if (it == list->end()) return false;
    const size_t from = static_cast<size_t>(std::distance(list->begin(), it));
    const size_t to = std::min(newIndex, list->size() - 1);
    if (from == to) return false;
    AuxSend moved = std::move(*it);
    list->erase(it);
    list->insert(list->begin() + static_cast<ptrdiff_t>(to), std::move(moved));
    notifyChanged();
    return true;
}

bool Edit::restoreSend_(const std::string& ownerId, AuxSend send, size_t index) {
    auto* list = sendsFor(*this, ownerId);
    if (!list || send.id.empty()) return false;
    if (std::any_of(list->begin(), list->end(), [&](const AuxSend& existing) {
            return existing.id == send.id;
        })) return false;
    const size_t at = std::min(index, list->size());
    list->insert(list->begin() + static_cast<ptrdiff_t>(at), std::move(send));
    if (!validateRouting().ok) {
        list->erase(list->begin() + static_cast<ptrdiff_t>(at));
        return false;
    }
    normalizeChainFor_(ownerId);
    notifyChanged();
    return true;
}

bool Edit::updateSend(const std::string& ownerId, const AuxSend& send) {
    auto* list = sendsFor(*this, ownerId);
    if (!list) return false;
    for (auto& existing : *list) {
        if (existing.id != send.id) continue;
        const AuxSend previous = existing;
        existing = send;
        if (!validateRouting().ok) {
            existing = previous;
            return false;
        }
        notifyChanged();
        return true;
    }
    return false;
}

bool Edit::removeSend(const std::string& ownerId, const std::string& sendId) {
    auto* list = sendsFor(*this, ownerId);
    if (!list) return false;
    for (auto it = list->begin(); it != list->end(); ++it) {
        if (it->id != sendId) continue;
        list->erase(it);
        normalizeChainFor_(ownerId);
        notifyChanged();
        return true;
    }
    return false;
}

std::string Edit::addMidiClip(const std::string& trackId, MidiClip clip) {
    Track* mt = track(trackId);
    if (mt == nullptr) return "";
    clip.id = newId("mclip_");
    ++idCounter_;
    mt->midiClips.push_back(std::move(clip));
    notifyChanged();
    return mt->midiClips.back().id;
}

MidiClip* Edit::midiClip(const std::string& trackId, const std::string& clipId) {
    Track* mt = track(trackId);
    if (mt == nullptr) return nullptr;
    for (auto& c : mt->midiClips) if (c.id == clipId) return &c;
    return nullptr;
}

bool Edit::removeMidiClip(const std::string& trackId, const std::string& clipId) {
    Track* mt = track(trackId);
    if (mt == nullptr) return false;
    for (auto it = mt->midiClips.begin(); it != mt->midiClips.end(); ++it) {
        if (it->id == clipId) {
            mt->midiClips.erase(it);
            pruneClipGroups_();
            notifyChanged();
            return true;
        }
    }
    return false;
}

bool Edit::setMidiInstrument(const std::string& trackId, PluginSlot slot) {
    Track* mt = track(trackId);
    if (mt == nullptr) return false;
    // The instrument keeps a stable id across replacements only if the caller
    // supplies one; a fresh instrument gets a new id so the GraphBuilder's
    // instance cache can't hand back the previous plugin.
    if (slot.id.empty() && !slot.uidString.empty()) {
        slot.id = newId("instrument_");
        ++idCounter_;
    }
    mt->instrument = std::move(slot);
    notifyChanged();
    return true;
}

bool Edit::anySoloed() const {
    for (const auto& t : tracks_) if (t.solo) return true;
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
        for (const auto& c : t.midiClips) {
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
