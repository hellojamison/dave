// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/ProjectFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dave::document {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─── Kind/mode string helpers ───────────────────────────────────────────────

static const char* kindStr(MarkerKind k) {
    switch (k) {
        case MarkerKind::Cue:     return "cue";
        case MarkerKind::Section: return "section";
        case MarkerKind::Loop:    return "loop";
        case MarkerKind::Punch:   return "punch";
        case MarkerKind::CD:      return "cd";
        default:                  return "custom";
    }
}
static MarkerKind parseKind(const std::string& s) {
    if (s == "cue")     return MarkerKind::Cue;
    if (s == "section") return MarkerKind::Section;
    if (s == "loop")    return MarkerKind::Loop;
    if (s == "punch")   return MarkerKind::Punch;
    if (s == "cd")      return MarkerKind::CD;
    return MarkerKind::Custom;
}
static const char* modeStr(MarkerPosMode m) {
    switch (m) {
        case MarkerPosMode::Sample:       return "sample";
        case MarkerPosMode::Smpte:        return "smpte";
        case MarkerPosMode::Musical:      return "musical";
        case MarkerPosMode::ClipAnchored: return "clipAnchored";
    }
    return "sample";
}
static MarkerPosMode parseMode(const std::string& s) {
    if (s == "smpte")        return MarkerPosMode::Smpte;
    if (s == "musical")      return MarkerPosMode::Musical;
    if (s == "clipAnchored") return MarkerPosMode::ClipAnchored;
    return MarkerPosMode::Sample;
}

// ─── Plugin slots ───────────────────────────────────────────────────────────
// Slots appear in three places now (audio chains, MIDI instruments, MIDI
// chains), so the field list lives once rather than three times.

static json pluginSlotToJson(const PluginSlot& p) {
    return json{
        {"id", p.id},
        {"name", p.name},
        {"uidString", p.uidString},
        {"path", p.path},
        {"bypass", p.bypass},
        {"stateBase64", p.stateBase64},
    };
}

static PluginSlot pluginSlotFromJson(const json& jp) {
    PluginSlot p;
    p.id = jp.value("id", "");
    p.name = jp.value("name", "");
    p.uidString = jp.value("uidString", "");
    p.path = jp.value("path", "");
    p.bypass = jp.value("bypass", false);
    p.stateBase64 = jp.value("stateBase64", "");
    return p;
}

static const char* routeKindStr(RouteTarget::Kind kind) {
    switch (kind) {
        case RouteTarget::Kind::None: return "none";
        case RouteTarget::Kind::HardwareOutput: return "hardware";
        case RouteTarget::Kind::AudioTrack: return "audioTrack";
        case RouteTarget::Kind::Bus: return "bus";
    }
    return "none";
}

static RouteTarget::Kind parseRouteKind(const std::string& value) {
    if (value == "hardware") return RouteTarget::Kind::HardwareOutput;
    if (value == "audioTrack") return RouteTarget::Kind::AudioTrack;
    if (value == "bus") return RouteTarget::Kind::Bus;
    return RouteTarget::Kind::None;
}

static json routeToJson(const RouteTarget& route) {
    json result{{"kind", routeKindStr(route.kind)}};
    if (route.kind == RouteTarget::Kind::HardwareOutput) {
        result["firstChannel"] = route.hardware.firstChannel;
        result["channelCount"] = route.hardware.channelCount;
    } else if (route.kind == RouteTarget::Kind::AudioTrack ||
               route.kind == RouteTarget::Kind::Bus) {
        result["targetId"] = route.targetId;
    }
    return result;
}

static RouteTarget routeFromJson(const json& value, RouteTarget fallback) {
    if (!value.is_object()) return fallback;
    RouteTarget result;
    result.kind = parseRouteKind(value.value("kind", "none"));
    if (result.kind == RouteTarget::Kind::HardwareOutput) {
        result.targetId.clear();
        result.hardware.firstChannel = value.value("firstChannel", 0);
        result.hardware.channelCount = value.value("channelCount", 2);
    } else if (result.kind == RouteTarget::Kind::AudioTrack ||
               result.kind == RouteTarget::Kind::Bus) {
        result.targetId = value.value("targetId", "");
    } else {
        result = RouteTarget::none();
    }
    return result;
}

static json sendToJson(const AuxSend& send) {
    return json{{"id", send.id},
                {"target", routeToJson(send.target)},
                {"tap", send.tap == SendTap::PreFader ? "preFader" : "postFader"},
                {"gain", send.gain},
                {"muted", send.muted}};
}

static AuxSend sendFromJson(const json& value) {
    AuxSend send;
    send.id = value.value("id", "");
    if (value.contains("target")) {
        send.target = routeFromJson(value["target"], RouteTarget::none());
    }
    send.tap = value.value("tap", "postFader") == "preFader"
        ? SendTap::PreFader : SendTap::PostFader;
    send.gain = std::max(0.0, value.value("gain", 0.0));
    send.muted = value.value("muted", true);
    return send;
}

static json volumeAutomationToJson(
    const std::vector<VolumeAutomationPoint>& points) {
    json result = json::array();
    for (const auto& point : points) {
        result.push_back({{"id", point.id},
                          {"sample", point.sample},
                          {"db", point.db}});
    }
    return result;
}

static std::vector<VolumeAutomationPoint> volumeAutomationFromJson(
    const json& value) {
    std::vector<VolumeAutomationPoint> result;
    if (!value.is_array()) return result;
    size_t fallbackId = 0;
    for (const auto& stored : value) {
        if (!stored.is_object() ||
            (stored.contains("id") && !stored["id"].is_string()) ||
            (stored.contains("sample") &&
             !stored["sample"].is_number_integer()) ||
            (stored.contains("db") && !stored["db"].is_number())) {
            continue;
        }
        VolumeAutomationPoint point;
        point.id = stored.value("id", "");
        if (point.id.empty()) {
            point.id = "automation_loaded_" + std::to_string(++fallbackId);
        }
        point.sample = std::max<int64_t>(
            0, stored.value("sample", int64_t(0)));
        point.db = clampVolumeAutomationDb(stored.value("db", 0.0));
        const bool duplicate = std::any_of(
            result.begin(), result.end(), [&](const auto& existing) {
                return existing.id == point.id ||
                       existing.sample == point.sample;
            });
        if (!duplicate) result.push_back(std::move(point));
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    return result;
}

static json panAutomationToJson(
    const std::vector<PanAutomationPoint>& points) {
    json result = json::array();
    for (const auto& point : points) {
        result.push_back({{"id", point.id},
                          {"sample", point.sample},
                          {"pan", point.pan}});
    }
    return result;
}

static std::vector<PanAutomationPoint> panAutomationFromJson(
    const json& value) {
    std::vector<PanAutomationPoint> result;
    if (!value.is_array()) return result;
    size_t fallbackId = 0;
    for (const auto& stored : value) {
        if (!stored.is_object() ||
            (stored.contains("id") && !stored["id"].is_string()) ||
            (stored.contains("sample") &&
             !stored["sample"].is_number_integer()) ||
            (stored.contains("pan") && !stored["pan"].is_number())) {
            continue;
        }
        PanAutomationPoint point;
        point.id = stored.value("id", "");
        if (point.id.empty()) {
            point.id = "pan_automation_loaded_" +
                       std::to_string(++fallbackId);
        }
        point.sample = std::max<int64_t>(
            0, stored.value("sample", int64_t(0)));
        point.pan = clampPanAutomation(stored.value("pan", 0.0));
        const bool duplicate = std::any_of(
            result.begin(), result.end(), [&](const auto& existing) {
                return existing.id == point.id ||
                       existing.sample == point.sample;
            });
        if (!duplicate) result.push_back(std::move(point));
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.sample < b.sample;
              });
    return result;
}

template <typename Channel>
static void writeChannelRouting(json& value, const Channel& channel) {
    value["mainOutput"] = routeToJson(channel.mainOutput);
    json sends = json::array();
    for (const auto& send : channel.sends) sends.push_back(sendToJson(send));
    value["sends"] = std::move(sends);
    value["volumeAutomation"] =
        volumeAutomationToJson(channel.volumeAutomation);
    value["panAutomation"] = panAutomationToJson(channel.panAutomation);
}

template <typename Channel>
static void readChannelRouting(const json& value, Channel& channel) {
    if (value.contains("mainOutput")) {
        channel.mainOutput = routeFromJson(value["mainOutput"], RouteTarget::bus());
    }
    if (value.contains("sends") && value["sends"].is_array()) {
        for (const auto& send : value["sends"]) {
            channel.sends.push_back(sendFromJson(send));
        }
    }
    if (value.contains("volumeAutomation")) {
        channel.volumeAutomation =
            volumeAutomationFromJson(value["volumeAutomation"]);
    }
    if (value.contains("panAutomation")) {
        channel.panAutomation =
            panAutomationFromJson(value["panAutomation"]);
    }
}

// ─── Serialize ──────────────────────────────────────────────────────────────

std::string serializeEdit(const Edit& edit) {
    json j;
    j["format"] = "dave.doc/v3";
    // The session's own format, not a constant: sample positions in this
    // document are only meaningful against the rate that produced them.
    j["sampleRate"] = edit.sampleRate();
    j["bitDepth"] = edit.bitDepth();

    // Tracks (audio + clips + plugins).
    json tracks = json::array();
    for (const auto& t : edit.tracks()) {
        json jt;
        jt["id"] = t.id;
        jt["name"] = t.name;
        jt["color"] = t.color;
        jt["gain"] = t.gain;
        jt["pan"] = t.pan;
        jt["mute"] = t.mute;
        jt["solo"] = t.solo;
        jt["recordArm"] = t.recordArm;
        jt["inputChannel"] = t.inputChannel;
        jt["inputChannelCount"] = t.inputChannelCount;
        jt["hardwareInput"] = {
            // inputChannel/inputChannelCount remain a compatibility surface
            // during the v1->v2 transition. All new mutators synchronize the
            // pair, and preferring them here preserves older callers too.
            {"firstChannel", t.inputChannel},
            {"channelCount", t.inputChannelCount},
        };
        jt["inputMonitor"] = t.inputMonitor;
        writeChannelRouting(jt, t);
        json clips = json::array();
        for (const auto& c : t.clips) {
            clips.push_back({
                {"id", c.id},
                {"asset", c.asset.sha256},
                {"timelineStart", c.timelineStart},
                {"sourceOffset", c.sourceOffset},
                {"length", c.length},
                {"gain", c.gain},
                {"fadeIn", c.fadeIn},
                {"fadeOut", c.fadeOut},
            });
        }
        jt["clips"] = clips;
        json plugins = json::array();
        for (const auto& p : t.plugins) plugins.push_back(pluginSlotToJson(p));
        jt["plugins"] = plugins;
        tracks.push_back(jt);
    }
    j["tracks"] = tracks;

    // MIDI tracks (RB-7). Notes are written as fixed 5-element arrays rather
    // than objects: a few bars of piano is thousands of notes, and
    // {"startSample":...,"lengthSamples":...} per note turns a small project
    // into a megabyte of key names. The order is the MidiNote field order.
    json midiTracks = json::array();
    for (const auto& mt : edit.midiTracks()) {
        json jmt;
        jmt["id"] = mt.id;
        jmt["name"] = mt.name;
        jmt["color"] = mt.color;
        jmt["gain"] = mt.gain;
        jmt["pan"] = mt.pan;
        jmt["mute"] = mt.mute;
        jmt["solo"] = mt.solo;
        writeChannelRouting(jmt, mt);
        jmt["instrument"] = pluginSlotToJson(mt.instrument);
        json mclips = json::array();
        for (const auto& c : mt.clips) {
            json notes = json::array();
            for (const auto& n : c.notes) {
                notes.push_back(json::array({n.startSample, n.lengthSamples,
                                             n.pitch, n.velocity, n.channel}));
            }
            json tempi = json::array();
            for (const auto& t : c.sourceTempi) {
                tempi.push_back(json::array({t.tick, t.microsecondsPerQuarter}));
            }
            mclips.push_back({
                {"id", c.id},
                {"name", c.name},
                {"timelineStart", c.timelineStart},
                {"sourceOffset", c.sourceOffset},
                {"length", c.length},
                {"notes", notes},
                {"sourcePath", c.sourcePath},
                {"sourcePpq", c.sourcePpq},
                {"sourceTempi", tempi},
            });
        }
        jmt["clips"] = mclips;
        json mplugins = json::array();
        for (const auto& p : mt.plugins) mplugins.push_back(pluginSlotToJson(p));
        jmt["plugins"] = mplugins;
        midiTracks.push_back(jmt);
    }
    j["midiTracks"] = midiTracks;

    json buses = json::array();
    for (const auto& bus : edit.buses()) {
        json jb{{"id", bus.id},
                {"name", bus.name},
                {"color", bus.color},
                {"gain", bus.gain},
                {"pan", bus.pan},
                {"mute", bus.mute},
                {"solo", bus.solo},
                {"isMain", bus.isMain}};
        writeChannelRouting(jb, bus);
        json plugins = json::array();
        for (const auto& plugin : bus.plugins) {
            plugins.push_back(pluginSlotToJson(plugin));
        }
        jb["plugins"] = std::move(plugins);
        buses.push_back(std::move(jb));
    }
    j["buses"] = std::move(buses);

    // Assets.
    json assets = json::array();
    for (const auto& [id, a] : edit.assets()) {
        assets.push_back({
            {"id", a.id.sha256},
            {"path", a.path},
            {"sampleRate", a.sampleRate},
            {"channels", a.channels},
            {"lengthSamples", a.lengthSamples},
        });
    }
    j["assets"] = assets;

    // Marker tracks.
    json mtracks = json::array();
    for (const auto& mt : edit.markerTracks()) {
        json jmt;
        jmt["id"] = mt.id;
        jmt["name"] = mt.name;
        jmt["visible"] = mt.visible;
        json markers = json::array();
        for (const auto& m : mt.markers) {
            markers.push_back({
                {"id", m.id},
                {"name", m.name},
                {"kind", kindStr(m.kind)},
                {"posMode", modeStr(m.posMode)},
                {"position", m.position},
                {"length", m.length},
                {"color", m.color},
                {"clipId", m.clipId},
                {"metadata", m.metadata},
            });
        }
        jmt["markers"] = markers;
        mtracks.push_back(jmt);
    }
    j["markerTracks"] = mtracks;

    // Video tracks (RB-6).
    json vtracks = json::array();
    for (const auto& vt : edit.videoTracks()) {
        json jvt;
        jvt["id"] = vt.id;
        jvt["name"] = vt.name;
        jvt["visible"] = vt.visible;
        json vclips = json::array();
        for (const auto& c : vt.clips) {
            vclips.push_back({
                {"id", c.id},
                {"path", c.path},
                {"name", c.name},
                {"codec", c.codec},
                {"timelineStart", c.timelineStart},
                {"sourceOffset", c.sourceOffset},
                {"length", c.length},
                {"fps", c.fps},
                {"width", c.width},
                {"height", c.height},
                {"durationSeconds", c.durationSeconds},
            });
        }
        jvt["clips"] = vclips;
        vtracks.push_back(jvt);
    }
    j["videoTracks"] = vtracks;

    return j.dump(2);
}

// ─── Deserialize ────────────────────────────────────────────────────────────

ProjectResult deserializeEdit(const std::string& text, Edit& edit) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        return {false, std::string("JSON parse error: ") + e.what()};
    }
    const std::string format = j.value("format", "");
    const bool legacyV1 = format == "dave.doc/v1";
    if (!legacyV1 && format != "dave.doc/v2" && format != "dave.doc/v3") {
        return {false,
                "not a supported dave.doc project (expected v1, v2, or v3)"};
    }

    // Projects written before the session format was configurable carry no
    // bitDepth and a hardcoded 48000; both defaults match what they meant.
    edit.loadSessionFormat_(j.value("sampleRate", 48000), j.value("bitDepth", 24));

    // We rebuild the Edit by mutating it directly (its public mutators fire
    // notifyChanged, which we don't want during load — so use tracksMut() etc.
    // and DON'T fire the change listener until the end). For simplicity we
    // suppress by direct field access via the mut- accessors.
    auto& tracks = edit.tracksMut();
    tracks.clear();
    if (j.contains("tracks")) {
        for (const auto& jt : j["tracks"]) {
            Track t;
            t.id = jt.value("id", "");
            t.name = jt.value("name", "");
            t.color = jt.value("color", "");
            if (!validTrackColor(t.color)) t.color.clear();
            t.gain = jt.value("gain", 1.0);
            t.pan = jt.value("pan", 0.0);
            // Defaults keep projects written before mute/solo existed loading
            // as fully audible rather than silent.
            t.mute = jt.value("mute", false);
            t.solo = jt.value("solo", false);
            // Legacy projects were never armed and implicitly used the first
            // mono input. Only lower bounds are knowable here; the eventual
            // capture device supplies the upper bound to
            // clampTrackInputToCaptureChannels().
            t.recordArm = jt.value("recordArm", false);
            t.inputChannel = std::max(0, jt.value("inputChannel", 0));
            t.inputChannelCount =
                std::max(1, jt.value("inputChannelCount", 1));
            t.hardwareInput = {t.inputChannel, t.inputChannelCount};
            if (!legacyV1 && jt.contains("hardwareInput")) {
                const auto& input = jt["hardwareInput"];
                t.hardwareInput.firstChannel =
                    std::max(0, input.value("firstChannel", t.inputChannel));
                t.hardwareInput.channelCount =
                    std::clamp(input.value("channelCount", t.inputChannelCount), 1, 2);
                t.inputChannel = t.hardwareInput.firstChannel;
                t.inputChannelCount = t.hardwareInput.channelCount;
            }
            t.inputMonitor = legacyV1 ? false : jt.value("inputMonitor", false);
            if (!legacyV1) readChannelRouting(jt, t);
            if (jt.contains("clips")) {
                for (const auto& jc : jt["clips"]) {
                    AudioClip c;
                    c.id = jc.value("id", "");
                    c.asset = AssetId{jc.value("asset", "")};
                    c.timelineStart = jc.value("timelineStart", int64_t(0));
                    c.sourceOffset = jc.value("sourceOffset", int64_t(0));
                    c.length = jc.value("length", int64_t(0));
                    c.gain = jc.value("gain", 1.0);
                    c.fadeIn = jc.value("fadeIn", int64_t(0));
                    c.fadeOut = jc.value("fadeOut", int64_t(0));
                    t.clips.push_back(std::move(c));
                }
            }
            if (jt.contains("plugins")) {
                for (const auto& jp : jt["plugins"]) {
                    t.plugins.push_back(pluginSlotFromJson(jp));
                }
            }
            tracks.push_back(std::move(t));
        }
    }

    // MIDI tracks (RB-7). Guarded by contains() like every other section, so a
    // project written before MIDI existed loads unchanged.
    edit.clearMidiTracks_();
    if (j.contains("midiTracks")) {
        for (const auto& jmt : j["midiTracks"]) {
            MidiTrack mt;
            mt.id = jmt.value("id", "");
            mt.name = jmt.value("name", "");
            mt.color = jmt.value("color", "");
            if (!validTrackColor(mt.color)) mt.color.clear();
            mt.gain = jmt.value("gain", 1.0);
            mt.pan = jmt.value("pan", 0.0);
            mt.mute = jmt.value("mute", false);
            mt.solo = jmt.value("solo", false);
            if (!legacyV1) readChannelRouting(jmt, mt);
            if (jmt.contains("instrument")) {
                mt.instrument = pluginSlotFromJson(jmt["instrument"]);
            }
            if (jmt.contains("clips")) {
                for (const auto& jc : jmt["clips"]) {
                    MidiClip c;
                    c.id = jc.value("id", "");
                    c.name = jc.value("name", "");
                    c.timelineStart = jc.value("timelineStart", int64_t(0));
                    c.sourceOffset = jc.value("sourceOffset", int64_t(0));
                    c.length = jc.value("length", int64_t(0));
                    c.sourcePath = jc.value("sourcePath", "");
                    c.sourcePpq = jc.value("sourcePpq", 480);
                    if (jc.contains("notes")) {
                        for (const auto& jn : jc["notes"]) {
                            // Skip anything that isn't a well-formed 5-tuple
                            // rather than throwing: one bad note shouldn't cost
                            // the user the whole project.
                            if (!jn.is_array() || jn.size() < 5) continue;
                            MidiNote n;
                            n.startSample = jn[0].get<int64_t>();
                            n.lengthSamples = jn[1].get<int64_t>();
                            n.pitch = static_cast<uint8_t>(jn[2].get<int>() & 0x7F);
                            n.velocity = static_cast<uint8_t>(jn[3].get<int>() & 0x7F);
                            n.channel = static_cast<uint8_t>(jn[4].get<int>() & 0x0F);
                            c.notes.push_back(n);
                        }
                    }
                    if (jc.contains("sourceTempi")) {
                        for (const auto& jt2 : jc["sourceTempi"]) {
                            if (!jt2.is_array() || jt2.size() < 2) continue;
                            TempoEvent te;
                            te.tick = jt2[0].get<int64_t>();
                            te.microsecondsPerQuarter = jt2[1].get<int32_t>();
                            c.sourceTempi.push_back(te);
                        }
                    }
                    mt.clips.push_back(std::move(c));
                }
            }
            if (jmt.contains("plugins")) {
                for (const auto& jp : jmt["plugins"]) {
                    mt.plugins.push_back(pluginSlotFromJson(jp));
                }
            }
            edit.loadMidiTrack_(std::move(mt));
        }
    }

    edit.clearBuses_();
    if (!legacyV1 && j.contains("buses") && j["buses"].is_array()) {
        for (const auto& jb : j["buses"]) {
            BusTrack bus;
            bus.id = jb.value("id", "");
            bus.name = jb.value("name", "");
            bus.color = jb.value("color", "");
            if (!validTrackColor(bus.color)) bus.color.clear();
            bus.gain = jb.value("gain", 1.0);
            bus.pan = jb.value("pan", 0.0);
            bus.mute = jb.value("mute", false);
            bus.solo = jb.value("solo", false);
            bus.isMain = bus.id == kMainBusId && jb.value("isMain", true);
            readChannelRouting(jb, bus);
            if (jb.contains("plugins")) {
                for (const auto& plugin : jb["plugins"]) {
                    bus.plugins.push_back(pluginSlotFromJson(plugin));
                }
            }
            edit.loadBus_(std::move(bus));
        }
    }
    edit.ensureMainBus_();

    // Assets — re-add via the asset map. We can't reach the private map, so we
    // re-import by path (which re-hashes; for bundled files the hash matches
    // and dedupes). For load we skip re-hashing and just reconstruct the map
    // entries from the JSON. Use a friend-style trick: the Edit's importAsset
    // is the public path; but during load we don't want to re-copy files. So
    // we populate AudioAsset entries by direct reconstruction. Since assets_
    // is private, we expose a load-time helper.
    // (See Edit::loadAsset_ — added below.)
    if (j.contains("assets")) {
        for (const auto& ja : j["assets"]) {
            AudioAsset a;
            a.id = AssetId{ja.value("id", "")};
            a.path = ja.value("path", "");
            a.sampleRate = ja.value("sampleRate", 0);
            a.channels = ja.value("channels", 0);
            a.lengthSamples = ja.value("lengthSamples", int64_t(0));
            edit.loadAsset_(std::move(a));
        }
    }

    // Marker tracks.
    edit.clearMarkerTracks_();
    if (j.contains("markerTracks")) {
        for (const auto& jmt : j["markerTracks"]) {
            MarkerTrack mt;
            mt.id = jmt.value("id", "");
            mt.name = jmt.value("name", "");
            mt.visible = jmt.value("visible", true);
            if (jmt.contains("markers")) {
                for (const auto& jm : jmt["markers"]) {
                    Marker m;
                    m.id = jm.value("id", "");
                    m.name = jm.value("name", "");
                    m.kind = parseKind(jm.value("kind", "cue"));
                    m.posMode = parseMode(jm.value("posMode", "sample"));
                    m.position = jm.value("position", int64_t(0));
                    m.length = jm.value("length", int64_t(0));
                    m.color = jm.value("color", "");
                    m.clipId = jm.value("clipId", "");
                    m.metadata = jm.value("metadata", "");
                    mt.markers.push_back(std::move(m));
                }
            }
            edit.loadMarkerTrack_(std::move(mt));
        }
    }

    // Video tracks (RB-6).
    edit.clearVideoTracks_();
    if (j.contains("videoTracks")) {
        for (const auto& jvt : j["videoTracks"]) {
            VideoTrack vt;
            vt.id = jvt.value("id", "");
            vt.name = jvt.value("name", "");
            vt.visible = jvt.value("visible", true);
            if (jvt.contains("clips")) {
                for (const auto& jc : jvt["clips"]) {
                    VideoClip c;
                    c.id = jc.value("id", "");
                    c.path = jc.value("path", "");
                    c.name = jc.value("name", "");
                    c.codec = jc.value("codec", "");
                    c.timelineStart = jc.value("timelineStart", int64_t(0));
                    c.sourceOffset = jc.value("sourceOffset", int64_t(0));
                    c.length = jc.value("length", int64_t(0));
                    c.fps = jc.value("fps", 0.0);
                    c.width = jc.value("width", 0);
                    c.height = jc.value("height", 0);
                    c.durationSeconds = jc.value("durationSeconds", 0.0);
                    vt.clips.push_back(std::move(c));
                }
            }
            edit.loadVideoTrack_(std::move(vt));
        }
    }

    const auto routing = edit.validateRouting();
    if (!routing.ok) return {false, "invalid routing: " + routing.message};
    return {true, ""};
}

// ─── Bundle save/load (with media copy) ─────────────────────────────────────

bool isDaveBundle(const std::string& path) {
    return path.size() > 5 && path.substr(path.size() - 5) == ".dave" &&
           fs::is_directory(path);
}

namespace {

bool isPathWithin(const fs::path& child, const fs::path& parent) {
    std::error_code error;
    const fs::path childPath = fs::weakly_canonical(child, error);
    if (error) return false;
    const fs::path parentPath = fs::weakly_canonical(parent, error);
    if (error) return false;
    auto childIt = childPath.begin();
    for (auto parentIt = parentPath.begin(); parentIt != parentPath.end();
         ++parentIt, ++childIt) {
        if (childIt == childPath.end() || *childIt != *parentIt) return false;
    }
    return true;
}

bool looksLikeBundledRecording(const fs::path& path) {
    const fs::path parent = path.parent_path();
    return parent.filename() == "recordings" &&
           parent.parent_path().extension() == ".dave";
}

} // namespace

ProjectResult saveBundle(const std::string& bundlePath, const Edit& edit,
                         bool copyAssets) {
    try {
        fs::create_directories(bundlePath);
        fs::path assetsDir = fs::path(bundlePath) / "assets";
        fs::path videoDir = fs::path(bundlePath) / "video";
        fs::path recordingsDir = fs::path(bundlePath) / "recordings";
        if (copyAssets) {
            fs::create_directories(assetsDir);
            fs::create_directories(videoDir);
            fs::create_directories(recordingsDir);
        }

        // For saveBundle we write the JSON with asset paths rewritten to be
        // relative to the bundle (assets/<sha>) when copyAssets is true. The
        // serializer above writes original paths; we post-process by building
        // a temporary Edit copy with rewritten paths. Simpler: re-emit JSON
        // here with the rewrite.
        json j = json::parse(serializeEdit(edit));
        if (copyAssets) {
            // Copy + rewrite audio assets.
            if (j.contains("assets")) {
                for (auto& ja : j["assets"]) {
                    std::string orig = ja.value("path", "");
                    std::string sha = ja.value("id", "");
                    if (!orig.empty() && fs::exists(orig)) {
                        // Determine extension from original.
                        fs::path src(orig);
                        // Finished takes already inside this bundle are not
                        // copied into assets/: retain their human-readable
                        // filename and only make the JSON path relative. Save
                        // As copies takes from the old bundle's recordings
                        // directory into the new one.
                        if (isPathWithin(src, recordingsDir) ||
                            looksLikeBundledRecording(src)) {
                            fs::path dst = recordingsDir / src.filename();
                            if (!isPathWithin(src, recordingsDir)) {
                                const std::string stem = dst.stem().string();
                                const std::string extension =
                                    dst.extension().string();
                                for (int suffix = 2; fs::exists(dst);
                                     ++suffix) {
                                    dst = recordingsDir /
                                        (stem + "_" +
                                         std::to_string(suffix) + extension);
                                }
                                fs::copy_file(src, dst);
                            }
                            ja["path"] =
                                "recordings/" + dst.filename().string();
                            continue;
                        }
                        std::string ext = src.extension().string();
                        fs::path dst = assetsDir / (sha + ext);
                        if (!fs::exists(dst)) {
                            fs::copy_file(orig, dst,
                                          fs::copy_options::overwrite_existing);
                        }
                        ja["path"] = "assets/" + sha + ext;
                    }
                }
            }
            // Copy + rewrite video clips (RB-6: multiple clips across tracks).
            if (j.contains("videoTracks")) {
                for (auto& jvt : j["videoTracks"]) {
                    if (!jvt.contains("clips")) continue;
                    for (auto& jc : jvt["clips"]) {
                        std::string orig = jc.value("path", "");
                        if (!orig.empty() && fs::exists(orig)) {
                            fs::path src(orig);
                            fs::path dst = videoDir / src.filename();
                            if (!fs::exists(dst)) {
                                fs::copy_file(orig, dst,
                                              fs::copy_options::overwrite_existing);
                            }
                            jc["path"] = "video/" + src.filename().string();
                        }
                    }
                }
            }
        }

        // Atomic write of project.json.
        fs::path jsonPath = fs::path(bundlePath) / "project.json";
        fs::path tmpPath = fs::path(bundlePath) / "project.json.tmp";
        {
            std::ofstream out(tmpPath);
            if (!out) return {false, "cannot open project.json.tmp for write"};
            out << j.dump(2);
            out.flush();
        }
        fs::rename(tmpPath, jsonPath);
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string("saveBundle error: ") + e.what()};
    }
}

ProjectResult loadBundle(const std::string& bundlePath, Edit& edit) {
    try {
        fs::path jsonPath = fs::path(bundlePath) / "project.json";
        if (!fs::exists(jsonPath)) {
            return {false, "no project.json in bundle"};
        }
        std::ifstream in(jsonPath);
        if (!in) return {false, "cannot open project.json"};
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();

        auto r = deserializeEdit(text, edit);
        if (!r.ok) return r;

        // Rewrite relative media paths to absolute (bundle-rooted).
        fs::path base(bundlePath);
        for (auto& [id, a] : const_cast<std::unordered_map<AssetId, AudioAsset>&>(edit.assets())) {
            if (!a.path.empty() &&
                (a.path.find("assets/") == 0 ||
                 a.path.find("recordings/") == 0)) {
                a.path = (base / a.path).string();
            }
        }
        // Rewrite relative video paths to absolute (bundle-rooted).
        for (auto& vt : edit.videoTracksMut()) {
            for (auto& c : vt.clips) {
                if (!c.path.empty() && c.path.find("video/") == 0) {
                    c.path = (base / c.path).string();
                }
            }
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string("loadBundle error: ") + e.what()};
    }
}

} // namespace dave::document
