// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/ProjectFile.h"

#include <nlohmann/json.hpp>

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

// ─── Serialize ──────────────────────────────────────────────────────────────

std::string serializeEdit(const Edit& edit) {
    json j;
    j["format"] = "dave.doc/v1";
    j["sampleRate"] = 48000; // RB single-rate

    // Tracks (audio + clips + plugins).
    json tracks = json::array();
    for (const auto& t : edit.tracks()) {
        json jt;
        jt["id"] = t.id;
        jt["name"] = t.name;
        jt["gain"] = t.gain;
        jt["pan"] = t.pan;
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
        for (const auto& p : t.plugins) {
            plugins.push_back({
                {"id", p.id},
                {"name", p.name},
                {"uidString", p.uidString},
                {"path", p.path},
                {"bypass", p.bypass},
                {"stateBase64", p.stateBase64},
            });
        }
        jt["plugins"] = plugins;
        tracks.push_back(jt);
    }
    j["tracks"] = tracks;

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
    if (j.value("format", "") != "dave.doc/v1") {
        return {false, "not a dave.doc/v1 project (format missing or wrong)"};
    }

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
            t.gain = jt.value("gain", 1.0);
            t.pan = jt.value("pan", 0.0);
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
                    PluginSlot p;
                    p.id = jp.value("id", "");
                    p.name = jp.value("name", "");
                    p.uidString = jp.value("uidString", "");
                    p.path = jp.value("path", "");
                    p.bypass = jp.value("bypass", false);
                    p.stateBase64 = jp.value("stateBase64", "");
                    t.plugins.push_back(std::move(p));
                }
            }
            tracks.push_back(std::move(t));
        }
    }

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

    return {true, ""};
}

// ─── Bundle save/load (with media copy) ─────────────────────────────────────

bool isDaveBundle(const std::string& path) {
    return path.size() > 5 && path.substr(path.size() - 5) == ".dave" &&
           fs::is_directory(path);
}

ProjectResult saveBundle(const std::string& bundlePath, const Edit& edit,
                         bool copyAssets) {
    try {
        fs::create_directories(bundlePath);
        fs::path assetsDir = fs::path(bundlePath) / "assets";
        fs::path videoDir = fs::path(bundlePath) / "video";
        if (copyAssets) {
            fs::create_directories(assetsDir);
            fs::create_directories(videoDir);
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

        // Rewrite relative asset/video paths to absolute (bundle-rooted).
        fs::path base(bundlePath);
        for (auto& [id, a] : const_cast<std::unordered_map<AssetId, AudioAsset>&>(edit.assets())) {
            if (!a.path.empty() && a.path.find("assets/") == 0) {
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
