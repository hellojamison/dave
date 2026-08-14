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
    if (auto* track = edit.midiTrack(id)) return &track->mainOutput;
    if (auto* bus = edit.bus(id)) return &bus->mainOutput;
    return nullptr;
}

std::vector<AuxSend>* sendsFor(Edit& edit, const std::string& id) {
    if (auto* track = edit.track(id)) return &track->sends;
    if (auto* track = edit.midiTrack(id)) return &track->sends;
    if (auto* bus = edit.bus(id)) return &bus->sends;
    return nullptr;
}

const std::vector<AuxSend>* sendsFor(const Edit& edit, const std::string& id) {
    if (const auto* track = edit.track(id)) return &track->sends;
    if (const auto* track = edit.midiTrack(id)) return &track->sends;
    if (const auto* bus = edit.bus(id)) return &bus->sends;
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
    if (bus(kMainBusId) != nullptr) return;
    BusTrack main;
    main.id = kMainBusId;
    main.name = "Main";
    main.isMain = true;
    main.mainOutput = RouteTarget::hardwareOutput(0, 2);
    buses_.push_back(std::move(main));
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
    if (routeReferences(RouteTarget::Kind::AudioTrack, id)) return false;
    for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
        if (it->id == id) {
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
    else if (auto* value = midiTrack(ownerId)) destination = &value->color;
    else if (auto* value = bus(ownerId)) destination = &value->color;
    if (destination == nullptr) return false;
    if (*destination == color) return true;
    *destination = std::move(color);
    notifyChanged();
    return true;
}

std::vector<VolumeAutomationPoint>* Edit::volumeAutomation(
    const std::string& ownerId) {
    if (auto* value = track(ownerId)) return &value->volumeAutomation;
    if (auto* value = midiTrack(ownerId)) return &value->volumeAutomation;
    if (auto* value = bus(ownerId)) return &value->volumeAutomation;
    return nullptr;
}

const std::vector<VolumeAutomationPoint>* Edit::volumeAutomation(
    const std::string& ownerId) const {
    if (const auto* value = track(ownerId)) return &value->volumeAutomation;
    if (const auto* value = midiTrack(ownerId)) return &value->volumeAutomation;
    if (const auto* value = bus(ownerId)) return &value->volumeAutomation;
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

std::vector<PanAutomationPoint>* Edit::panAutomation(
    const std::string& ownerId) {
    if (auto* value = track(ownerId)) return &value->panAutomation;
    if (auto* value = midiTrack(ownerId)) return &value->panAutomation;
    if (auto* value = bus(ownerId)) return &value->panAutomation;
    return nullptr;
}

const std::vector<PanAutomationPoint>* Edit::panAutomation(
    const std::string& ownerId) const {
    if (const auto* value = track(ownerId)) return &value->panAutomation;
    if (const auto* value = midiTrack(ownerId)) return &value->panAutomation;
    if (const auto* value = bus(ownerId)) return &value->panAutomation;
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
            notifyChanged();
            return true;
        }
    }
    return false;
}

std::string Edit::addPlugin(const std::string& trackId, PluginSlot slot) {
    std::vector<PluginSlot>* plugins = nullptr;
    if (Track* t = track(trackId)) plugins = &t->plugins;
    else if (BusTrack* b = bus(trackId)) plugins = &b->plugins;
    if (plugins == nullptr) return "";
    slot.id = newId("plugin_");
    ++idCounter_;
    plugins->push_back(std::move(slot));
    notifyChanged();
    return plugins->back().id;
}

bool Edit::removePlugin(const std::string& trackId, const std::string& slotId) {
    std::vector<PluginSlot>* plugins = nullptr;
    if (Track* t = track(trackId)) plugins = &t->plugins;
    else if (BusTrack* b = bus(trackId)) plugins = &b->plugins;
    if (plugins == nullptr) return false;
    for (auto it = plugins->begin(); it != plugins->end(); ++it) {
        if (it->id == slotId) {
            plugins->erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

// ─── MIDI tracks ────────────────────────────────────────────────────────────

std::string Edit::addMidiTrack(const std::string& name) {
    MidiTrack mt;
    mt.id = newId("miditrack_");
    ++idCounter_;
    mt.name = name.empty() ? ("MIDI " + std::to_string(idCounter_)) : name;
    midiTracks_.push_back(std::move(mt));
    notifyChanged();
    return midiTracks_.back().id;
}

MidiTrack* Edit::midiTrack(const std::string& id) {
    for (auto& mt : midiTracks_) if (mt.id == id) return &mt;
    return nullptr;
}

const MidiTrack* Edit::midiTrack(const std::string& id) const {
    for (const auto& mt : midiTracks_) if (mt.id == id) return &mt;
    return nullptr;
}

bool Edit::removeMidiTrack(const std::string& id) {
    for (auto it = midiTracks_.begin(); it != midiTracks_.end(); ++it) {
        if (it->id == id) {
            midiTracks_.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

// ─── Routing and buses ─────────────────────────────────────────────────────

BusTrack* Edit::bus(const std::string& id) {
    for (auto& candidate : buses_) if (candidate.id == id) return &candidate;
    return nullptr;
}

const BusTrack* Edit::bus(const std::string& id) const {
    for (const auto& candidate : buses_) if (candidate.id == id) return &candidate;
    return nullptr;
}

std::string Edit::addBus(const std::string& name) {
    BusTrack candidate;
    do {
        candidate.id = newId("bus_");
        ++idCounter_;
    } while (bus(candidate.id) || track(candidate.id) || midiTrack(candidate.id));
    candidate.name = name.empty() ? "Bus " + std::to_string(idCounter_) : name;
    candidate.mainOutput = RouteTarget::bus();
    const std::string id = candidate.id;
    auto main = std::find_if(buses_.begin(), buses_.end(), [](const BusTrack& bus) {
        return bus.id == kMainBusId;
    });
    buses_.insert(main, std::move(candidate));
    notifyChanged();
    return id;
}

bool Edit::restoreBus_(BusTrack restored, size_t index) {
    if (restored.id.empty() || restored.id == kMainBusId || bus(restored.id)) {
        return false;
    }
    const size_t mainIndex = buses_.empty() ? 0 : buses_.size() - 1;
    const size_t at = std::min(index, mainIndex);
    buses_.insert(buses_.begin() + static_cast<ptrdiff_t>(at),
                  std::move(restored));
    const auto validation = validateRouting();
    if (!validation.ok) {
        buses_.erase(buses_.begin() + static_cast<ptrdiff_t>(at));
        return false;
    }
    notifyChanged();
    return true;
}

bool Edit::removeBus(const std::string& id) {
    if (id == kMainBusId || routeReferences(RouteTarget::Kind::Bus, id)) {
        return false;
    }
    for (auto it = buses_.begin(); it != buses_.end(); ++it) {
        if (it->id != id) continue;
        buses_.erase(it);
        notifyChanged();
        return true;
    }
    return false;
}

bool Edit::routeReferences(RouteTarget::Kind kind, const std::string& id) const {
    const auto references = [&](const RouteTarget& target) {
        return target.kind == kind && target.targetId == id;
    };
    const auto channelReferences = [&](const auto& channel) {
        if (references(channel.mainOutput)) return true;
        return std::any_of(channel.sends.begin(), channel.sends.end(),
                           [&](const AuxSend& send) { return references(send.target); });
    };
    for (const auto& track : tracks_) if (channelReferences(track)) return true;
    for (const auto& track : midiTracks_) if (channelReferences(track)) return true;
    for (const auto& candidate : buses_) if (channelReferences(candidate)) return true;
    return false;
}

Edit::RoutingValidation Edit::validateRouting() const {
    std::unordered_set<std::string> audioIds;
    std::unordered_set<std::string> busIds;
    std::unordered_set<std::string> allOwnerIds;
    std::vector<std::string> owners;
    for (const auto& track : tracks_) {
        if (track.id.empty() || !audioIds.insert(track.id).second) {
            return {false, "audio track has an empty or duplicate id"};
        }
        if (!allOwnerIds.insert(track.id).second) {
            return {false, "routing owner ids must be globally unique"};
        }
        owners.push_back(track.id);
        if (!validHardwareSpan(track.hardwareInput, false)) {
            return {false, "audio track has an invalid hardware input span"};
        }
    }
    for (const auto& track : midiTracks_) {
        if (track.id.empty() || !allOwnerIds.insert(track.id).second) {
            return {false, "MIDI track has an empty or duplicate id"};
        }
        owners.push_back(track.id);
    }
    for (const auto& candidate : buses_) {
        if (candidate.id.empty() || !busIds.insert(candidate.id).second) {
            return {false, "bus has an empty or duplicate id"};
        }
        if (!allOwnerIds.insert(candidate.id).second) {
            return {false, "routing owner ids must be globally unique"};
        }
        owners.push_back(candidate.id);
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
    for (const auto& track : midiTracks_) {
        auto result = checkChannel(track); if (!result.ok) return result;
    }
    for (const auto& candidate : buses_) {
        auto result = checkChannel(candidate); if (!result.ok) return result;
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
        for (const auto& track : midiTracks_) if (has(track)) return true;
        for (const auto& bus : buses_) if (has(bus)) return true;
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
    notifyChanged();
    return list->back().id;
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
        notifyChanged();
        return true;
    }
    return false;
}

std::string Edit::addMidiClip(const std::string& trackId, MidiClip clip) {
    MidiTrack* mt = midiTrack(trackId);
    if (mt == nullptr) return "";
    clip.id = newId("mclip_");
    ++idCounter_;
    mt->clips.push_back(std::move(clip));
    notifyChanged();
    return mt->clips.back().id;
}

MidiClip* Edit::midiClip(const std::string& trackId, const std::string& clipId) {
    MidiTrack* mt = midiTrack(trackId);
    if (mt == nullptr) return nullptr;
    for (auto& c : mt->clips) if (c.id == clipId) return &c;
    return nullptr;
}

bool Edit::removeMidiClip(const std::string& trackId, const std::string& clipId) {
    MidiTrack* mt = midiTrack(trackId);
    if (mt == nullptr) return false;
    for (auto it = mt->clips.begin(); it != mt->clips.end(); ++it) {
        if (it->id == clipId) {
            mt->clips.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

bool Edit::setMidiInstrument(const std::string& trackId, PluginSlot slot) {
    MidiTrack* mt = midiTrack(trackId);
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

std::string Edit::addMidiPlugin(const std::string& trackId, PluginSlot slot) {
    MidiTrack* mt = midiTrack(trackId);
    if (mt == nullptr) return "";
    slot.id = newId("plugin_");
    ++idCounter_;
    mt->plugins.push_back(std::move(slot));
    notifyChanged();
    return mt->plugins.back().id;
}

bool Edit::removeMidiPlugin(const std::string& trackId, const std::string& slotId) {
    MidiTrack* mt = midiTrack(trackId);
    if (mt == nullptr) return false;
    for (auto it = mt->plugins.begin(); it != mt->plugins.end(); ++it) {
        if (it->id == slotId) {
            mt->plugins.erase(it);
            notifyChanged();
            return true;
        }
    }
    return false;
}

bool Edit::anySoloed() const {
    for (const auto& t : tracks_) if (t.solo) return true;
    for (const auto& mt : midiTracks_) if (mt.solo) return true;
    for (const auto& bus : buses_) if (bus.solo) return true;
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
    for (const auto& mt : midiTracks_) {
        for (const auto& c : mt.clips) {
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
