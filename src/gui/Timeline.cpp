// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/Timeline.h"
#include "editing/Commands.h"
#include "gui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dave::gui {

// Format a sample position into the selected timecode mode.
// sr=48000, fps=24 (default), bpm=120, beatsPerBar=4.
std::string formatTimecode(int64_t samples, TimecodeMode mode,
                           double sr, double fps, double bpm) {
    char buf[32];
    switch (mode) {
        case TimecodeMode::MinSec: {
            double sec = samples / sr;
            int mm = static_cast<int>(sec) / 60;
            double ss = sec - mm * 60;
            std::snprintf(buf, sizeof(buf), "%d:%05.2f", mm, ss);
            break;
        }
        case TimecodeMode::Smpte: {
            double totalSec = samples / sr;
            int hh = static_cast<int>(totalSec / 3600);
            int mm = static_cast<int>(totalSec / 60) % 60;
            int ss = static_cast<int>(totalSec) % 60;
            int ff = static_cast<int>((totalSec - static_cast<int>(totalSec)) * fps);
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
            break;
        }
        case TimecodeMode::BarsBeats: {
            double beatsPerSec = bpm / 60.0;
            double totalBeats = (samples / sr) * beatsPerSec;
            int bars = static_cast<int>(totalBeats / 4) + 1;  // 1-indexed
            int beats = static_cast<int>(totalBeats) % 4 + 1;
            int ticks = static_cast<int>((totalBeats - static_cast<int>(totalBeats)) * 960);
            std::snprintf(buf, sizeof(buf), "%d.%d.%03d", bars, beats, ticks);
            break;
        }
        case TimecodeMode::FeetFrames: {
            double totalFrames = (samples / sr) * fps;
            int feet = static_cast<int>(totalFrames / 16);
            int frames = static_cast<int>(totalFrames) % 16;
            std::snprintf(buf, sizeof(buf), "%d+%02d", feet, frames);
            break;
        }
        case TimecodeMode::Samples: {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(samples));
            break;
        }
    }
    return buf;
}

namespace {

// The divisions a format counts in, ascending, in samples. One definition
// serves both the grid lines and the snap increment, so a selection edge can
// never land somewhere the grid says is not a division.
std::vector<double> formatLadder(TimecodeMode mode, double sr, double fps,
                                 double bpm, int& subdivisions) {
    const double frame = (fps > 0.0) ? sr / fps : sr;
    std::vector<double> ladder;
    subdivisions = 5;
    switch (mode) {
        case TimecodeMode::MinSec: {
            for (double s : {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0,
                             10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0,
                             1800.0, 3600.0}) {
                ladder.push_back(s * sr);
            }
            break;
        }
        case TimecodeMode::Smpte: {
            // Frames first, then whole seconds — the two units a timecode
            // display actually shows.
            for (double f : {1.0, 2.0, 5.0, 10.0}) ladder.push_back(f * frame);
            for (double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0,
                             300.0, 600.0, 1800.0, 3600.0}) {
                ladder.push_back(s * sr);
            }
            break;
        }
        case TimecodeMode::BarsBeats: {
            // Sixteenth-note up to 128 bars, in 4/4 — the only meter Dave has
            // until a real tempo map lands (see MidiClip::sourceTempi).
            const double beat = (bpm > 0.0) ? sr * 60.0 / bpm : sr;
            for (double b : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
                             128.0, 256.0, 512.0, 1024.0}) {
                ladder.push_back(b * beat);
            }
            subdivisions = 4;   // four beats to the bar
            break;
        }
        case TimecodeMode::FeetFrames: {
            // 16 frames to the foot, so past one foot the ladder steps in feet.
            for (double f : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 80.0, 160.0,
                             400.0, 800.0, 1600.0, 8000.0, 16000.0}) {
                ladder.push_back(f * frame);
            }
            subdivisions = 4;
            break;
        }
        case TimecodeMode::Samples: {
            for (double decade = 1.0; decade <= 1e9; decade *= 10.0) {
                ladder.push_back(decade);
                ladder.push_back(decade * 2.0);
                ladder.push_back(decade * 5.0);
            }
            break;
        }
    }
    return ladder;
}

// The first division at least `targetPixels` wide, so both callers scale
// through the format's own units instead of falling back to raw samples.
double ladderStepFor(const std::vector<double>& ladder, double targetPixels,
                     double samplesPerPixel, double fallback) {
    const double wanted = targetPixels * std::max(1e-9, samplesPerPixel);
    for (double candidate : ladder) {
        if (candidate >= wanted) return candidate;
    }
    return ladder.empty() ? fallback : ladder.back();
}

} // namespace

void zoomAroundSample(TimelineViewState& view, double newSamplesPerPixel,
                      int64_t anchorSample) {
    view.samplesPerPixel = std::clamp(
        newSamplesPerPixel, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    // Before the first frame the widget has not reported its width yet, so
    // there is no "middle" to aim at. Change the zoom and leave the scroll
    // where it is rather than guessing at a centre.
    if (view.laneWidthPixels <= 0.0f) return;
    const double halfSpan =
        (view.laneWidthPixels * 0.5) * view.samplesPerPixel;
    view.scrollSamples =
        std::max(0.0, static_cast<double>(anchorSample) - halfSpan);
}

GridStep gridStepFor(TimecodeMode mode, double samplesPerPixel,
                     double sr, double fps, double bpm) {
    // Keep labelled divisions roughly a label-width apart whatever the format,
    // so the choice follows zoom rather than a fixed step that goes unreadable
    // at one end of the range and useless at the other.
    constexpr double kTargetMajorPixels = 120.0;
    const double frame = (fps > 0.0) ? sr / fps : sr;
    int subdivisions = 5;
    const std::vector<double> ladder =
        formatLadder(mode, sr, fps, bpm, subdivisions);
    const double major =
        ladderStepFor(ladder, kTargetMajorPixels, samplesPerPixel, sr);

    GridStep step;
    step.major = std::max<int64_t>(1, std::llround(major));
    if (mode == TimecodeMode::Smpte || mode == TimecodeMode::FeetFrames) {
        // A fifth of a second is 4.8 frames at 24 fps. Round the subdivision
        // down to whole frames so every line lands on one — a tick between
        // frames is a position the format cannot express.
        const double frames =
            std::max(1.0, std::floor((major / subdivisions) / frame));
        step.minor = std::max<int64_t>(1, std::llround(frames * frame));
    } else {
        step.minor = std::max<int64_t>(1, std::llround(major / subdivisions));
    }
    return step;
}

int64_t snapStepFor(TimecodeMode mode, double samplesPerPixel, double sr,
                    double fps, double bpm) {
    // Three pixels is about the finest a drag can be aimed at. Below that the
    // snap stops being a help and becomes a floor the pointer cannot reach
    // between; above it, the same ladder steps up in the format's own units,
    // so zooming out coarsens the snap to 2 frames, 5 frames, a second —
    // never to an arbitrary sample count.
    constexpr double kTargetSnapPixels = 3.0;
    int subdivisions = 5;
    const std::vector<double> ladder =
        formatLadder(mode, sr, fps, bpm, subdivisions);
    return std::max<int64_t>(1, std::llround(ladderStepFor(
        ladder, kTargetSnapPixels, samplesPerPixel, 1.0)));
}

// Helpers converting Palette ImVec4 -> ImU32 (ImDrawList wants packed colors).
static inline ImU32 C(const ImVec4& v) {
    return IM_COL32(
        int(v.x * 255), int(v.y * 255), int(v.z * 255), int(v.w * 255));
}

namespace {
constexpr size_t kPeakCacheBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxBucketsPerLevel = 1u << 20;

// ─── Track gutter (header column) ───────────────────────────────────────────
// Audio and MIDI tracks differ in what plays, not in how they are mixed, so
// the header column — name, mute/solo, gain, pan — is written once here rather
// than twice in drawTimeline where the two copies would immediately drift.

// Everything about the row that is the same for every track, computed once per
// frame instead of per row.
struct GutterLayout {
    ImDrawList* dl = nullptr;
    ImVec2 origin{0.0f, 0.0f};
    float gutterWidth = 260.0f;
    float rowPadding = 6.0f;
    float rowGap = 3.0f;
    float headerHeight = 0.0f;
    float labelHeight = 0.0f;
    float compactControlHeight = 0.0f;
    ImVec2 mouse{0.0f, 0.0f};
    bool gutterHovered = false;
};

// The mutable fields a gutter edits. Taken as pointers because the Edit is
// const here (the timeline is a pure function of the document) and the call
// sites already const_cast to write through.
struct TrackGutterFields {
    std::string* name = nullptr;
    double* gain = nullptr;
    double* pan = nullptr;
    bool* mute = nullptr;
    bool* solo = nullptr;
};

// Draws one track's header column. `uid` must be unique across ALL track types
// — it keys both the ImGui ids and the single inline-rename slot in the view
// state, so audio track 0 and MIDI track 0 must not collide.
// Returns the Y of the first free control row below pan, so a caller can add
// its own (the MIDI instrument row).
float drawTrackGutter(const GutterLayout& g, TimelineViewState& view,
                      document::Edit& edit, float y, int uid,
                      TrackGutterFields f, bool selected) {
    const auto& pal = theme::palette();
    ImDrawList* dl = g.dl;
    const ImVec2 origin = g.origin;

    // Double-click the name to rename (inline text input).
    {
        char renameId[32];
        std::snprintf(renameId, sizeof(renameId), "##rename_%d", uid);
        const bool thisRenaming = view.isRenaming && view.renameTrackIndex == uid;

        if (thisRenaming) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 20.0f, y + g.rowPadding));
            ImGui::PushItemWidth(g.gutterWidth - 88.0f);
            ImGui::SetKeyboardFocusHere();
            char nameBuf[128];
            std::strncpy(nameBuf, f.name->c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            ImGui::PushID(uid + 1000);
            if (ImGui::InputText(renameId, nameBuf, sizeof(nameBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                *f.name = nameBuf;
                edit.notifyChanged();
                view.isRenaming = false;
            }
            // Lose focus (click away / Escape) = cancel rename.
            if (!ImGui::IsItemActive() &&
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                 ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                view.isRenaming = false;
            }
            ImGui::PopID();
            ImGui::PopItemWidth();
        } else {
            ImFont* nameFont = theme::fonts().label != nullptr
                ? theme::fonts().label : ImGui::GetFont();
            dl->PushClipRect(
                ImVec2(origin.x + 20.0f, y),
                ImVec2(origin.x + g.gutterWidth - 62.0f,
                       y + g.headerHeight + g.rowPadding * 2.0f),
                true);
            dl->AddText(nameFont, g.labelHeight,
                        ImVec2(origin.x + 20.0f, y + g.rowPadding),
                        C(pal.text), f.name->c_str());
            dl->PopClipRect();
        }
        if (g.gutterHovered &&
            g.mouse.x >= origin.x + 16.0f &&
            g.mouse.x <= origin.x + g.gutterWidth - 62.0f &&
            g.mouse.y >= y &&
            g.mouse.y <= y + g.headerHeight + g.rowPadding * 2.0f &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            view.isRenaming = true;
            view.renameTrackIndex = uid;
        }
    }

    // Mute/solo, drawn to PTXExtractor's TrackStateIndicator spec: 21x17,
    // 3px radius, hairline border, amber for mute and yellow for solo.
    // Drawn rather than composed from ImGui::Button so the active fill can
    // be the state colour instead of the theme's control gradient.
    {
        const ImVec2 msSize(21.0f, 17.0f);
        const float msGap = 3.0f;
        const float msY = y + g.rowPadding;
        float msX = origin.x + g.gutterWidth - (msSize.x * 2.0f + msGap) - 12.0f;
        struct Toggle { const char* label; bool on; ImVec4 active; };
        const Toggle toggles[2] = {
            {"M", *f.mute, pal.trackMuteActive},
            {"S", *f.solo, pal.trackSoloActive},
        };
        ImGui::PushID(uid + 5000);
        for (int b = 0; b < 2; ++b) {
            ImGui::PushID(b);
            ImGui::SetCursorScreenPos(ImVec2(msX, msY));
            const bool pressed = ImGui::InvisibleButton("##ms", msSize);
            const bool hovered = ImGui::IsItemHovered();
            if (pressed) {
                if (b == 0) *f.mute = !*f.mute;
                else        *f.solo = !*f.solo;
                edit.notifyChanged();
            }
            const Toggle& t = toggles[b];
            const ImVec2 bMin(msX, msY);
            const ImVec2 bMax(msX + msSize.x, msY + msSize.y);
            ImU32 fill = t.on ? C(t.active)
                              : (hovered ? C(pal.surfaceStrong)
                                         : C(pal.trackControlInactive));
            dl->AddRectFilled(bMin, bMax, fill, 3.0f);
            dl->AddRect(bMin, bMax, t.on ? C(t.active) : C(pal.border), 3.0f);
            // Dark text on the bright active fills; both amber and yellow
            // are too light for the normal foreground to stay legible.
            const ImU32 labelCol =
                t.on ? IM_COL32(32, 30, 28, 255) : C(pal.textMuted);
            const ImVec2 ts = ImGui::CalcTextSize(t.label);
            dl->AddText(ImVec2(msX + (msSize.x - ts.x) * 0.5f,
                               msY + (msSize.y - ts.y) * 0.5f),
                        labelCol, t.label);
            if (hovered) {
                ImGui::SetTooltip("%s", b == 0 ? "Mute (Shift+M)"
                                               : "Solo (Shift+S)");
            }
            ImGui::PopID();
            msX += msSize.x + msGap;
        }
        ImGui::PopID();
    }

    // Interactive gain + pan controls.
    // Option-click (Alt-click) resets to default: gain=0 dB, pan=center.
    ImGui::PushID(uid);
    float controlY = y + g.rowPadding + g.headerHeight + g.rowGap;
    constexpr float controlX = 12.0f;
    constexpr float controlLabelW = 64.0f;
    const float sliderW = g.gutterWidth - controlX - 10.0f - controlLabelW - 6.0f;
    {
        // Gain: dB slider from -60 to +6 dB. Convert to/from linear.
        float gainDb = 20.0f * std::log10(
            std::max(0.0001f, static_cast<float>(*f.gain)));
        char gainText[16];
        std::snprintf(gainText, sizeof(gainText), "%.1f", gainDb);
        dl->AddText(ImVec2(origin.x + controlX, controlY + 1.0f),
                    C(pal.textMuted), "Gain");
        const float gainTextWidth = ImGui::CalcTextSize(gainText).x;
        dl->AddText(ImVec2(origin.x + controlX + controlLabelW - gainTextWidth - 4.0f,
                           controlY + 1.0f), C(pal.textSubtle), gainText);
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + controlX + controlLabelW, controlY));
        ImGui::PushItemWidth(sliderW);
        // The readout lives beside, not on, the grab so centre remains usable.
        if (ImGui::SliderFloat("##gain", &gainDb, -60.0f, 6.0f, "")) {
            *f.gain = std::pow(10.0f, gainDb / 20.0f);
            edit.notifyChanged();
        }
        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) {
            *f.gain = 1.0;
            edit.notifyChanged();
        }
        ImGui::PopItemWidth();
    }
    controlY += g.compactControlHeight + g.rowGap;
    // Pan: -1=L to +1=R. Option-click resets to center (0.0).
    {
        float panVal = static_cast<float>(*f.pan);
        const std::string panText = theme::formatPan(panVal);
        dl->AddText(ImVec2(origin.x + controlX, controlY + 1.0f),
                    C(pal.textMuted), "Pan");
        const float panTextWidth = ImGui::CalcTextSize(panText.c_str()).x;
        dl->AddText(ImVec2(origin.x + controlX + controlLabelW - panTextWidth - 4.0f,
                           controlY + 1.0f), C(pal.textSubtle), panText.c_str());
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + controlX + controlLabelW, controlY));
        ImGui::PushItemWidth(sliderW);
        if (ImGui::SliderFloat("##pan", &panVal, -1.0f, 1.0f, "")) {
            *f.pan = panVal;
            edit.notifyChanged();
        }
        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) {
            *f.pan = 0.0;
            edit.notifyChanged();
        }
        ImGui::PopItemWidth();
    }
    ImGui::PopID();
    return controlY + g.compactControlHeight + g.rowGap;
}

} // namespace

int PeakCache::bucketSizeFor(int samplesPerPixel, size_t sampleCount) {
    const int target = std::max(4, samplesPerPixel);
    int upper = 4;
    while (upper < target && upper < (1 << 29)) {
        upper <<= 1;
    }
    const int lower = std::max(4, upper >> 1);
    int bucketSize = target - lower <= upper - target ? lower : upper;

    // A very long asset must not let one fine mip level consume the whole
    // cache; increasing by powers of two keeps the selection predictable.
    while ((sampleCount + static_cast<size_t>(bucketSize) - 1) /
               static_cast<size_t>(bucketSize) > kMaxBucketsPerLevel &&
           bucketSize < (1 << 29)) {
        bucketSize <<= 1;
    }
    return bucketSize;
}

void PeakCache::trim(size_t incomingBytes) {
    while (cacheBytes_ + incomingBytes > kPeakCacheBytes && !cache_.empty()) {
        auto oldestAsset = cache_.end();
        size_t oldestLevel = 0;
        uint64_t oldestUse = std::numeric_limits<uint64_t>::max();
        for (auto asset = cache_.begin(); asset != cache_.end(); ++asset) {
            for (size_t level = 0; level < asset->second.size(); ++level) {
                if (asset->second[level].lastUsed < oldestUse) {
                    oldestAsset = asset;
                    oldestLevel = level;
                    oldestUse = asset->second[level].lastUsed;
                }
            }
        }
        if (oldestAsset == cache_.end()) {
            break;
        }
        cacheBytes_ -= oldestAsset->second[oldestLevel].peaks.buckets.size() *
                       sizeof(PeakBucket);
        oldestAsset->second.erase(oldestAsset->second.begin() +
                                  static_cast<ptrdiff_t>(oldestLevel));
        if (oldestAsset->second.empty()) {
            cache_.erase(oldestAsset);
        }
    }
}

const PeakLevel& PeakCache::get(const std::string& assetId,
                                const std::vector<std::vector<float>>& buffer,
                                int samplesPerPixel) {
    const size_t sampleCount = buffer.empty() ? 0 : buffer.front().size();
    const int bucketSize = bucketSizeFor(samplesPerPixel, sampleCount);
    if (auto asset = cache_.find(assetId); asset != cache_.end()) {
        for (auto& level : asset->second) {
            if (level.peaks.bucketSize == bucketSize) {
                level.lastUsed = ++useClock_;
                return level.peaks;
            }
        }
    }

    PeakLevel level;
    level.bucketSize = bucketSize;
    float peakMagnitude = 0.0f;
    if (!buffer.empty()) {
        const auto& ch = buffer.front();
        const int64_t total = static_cast<int64_t>(ch.size());
        const int64_t numBuckets = (total + bucketSize - 1) / bucketSize;
        level.buckets.reserve(static_cast<size_t>(numBuckets));
        for (int64_t bucket = 0; bucket < numBuckets; ++bucket) {
            const int64_t start = bucket * bucketSize;
            const int64_t end = std::min(start + bucketSize, total);
            float mn = 1e9f;
            float mx = -1e9f;
            for (int64_t sample = start; sample < end; ++sample) {
                mn = std::min(mn, ch[sample]);
                mx = std::max(mx, ch[sample]);
            }
            if (mn > mx) { mn = 0; mx = 0; }
            peakMagnitude = std::max(peakMagnitude, std::max(std::abs(mn), std::abs(mx)));
            level.buckets.push_back({mn, mx});
        }
    }
    // This is display-only gain: cap it so near-silent files do not turn
    // background noise into a dominant visual while normal recordings fill
    // the available waveform body.
    level.displayScale = std::min(8.0f, 1.0f / std::max(peakMagnitude, 0.001f));

    const size_t incomingBytes = level.buckets.size() * sizeof(PeakBucket);
    trim(incomingBytes);
    auto& levels = cache_[assetId];
    levels.push_back(CachedLevel{std::move(level), ++useClock_});
    cacheBytes_ += incomingBytes;
    return levels.back().peaks;
}

void drawTimeline(const document::Edit& edit,
                  editing::UndoStack& undo,
                  engine::Transport& transport,
                  PeakCache& peaks,
                  TimelineViewState& view,
                  const std::unordered_map<std::string,
                      std::vector<std::vector<float>>>& assetBuffers,
                  float trackHeight,
                  float timelineHeight) {
    // Draw directly into the host window's draw list (no child windows — they
    // introduce scrolling/sizing bugs that hid the clips in RB-2's first cut).
    // The gutter width follows the controls it contains; the remaining canvas
    // stays dedicated to clips, waveforms, and timeline interaction.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // 260 matches PTXExtractor's default track-header width, which is sized to
    // hold a name plus a full row of per-track controls without truncation.
    const float gutterWidth = 260.0f;
    const float totalWidth = avail.x;
    // Reported outward so keyboard zoom can re-centre (see zoomAroundSample).
    view.laneWidthPixels = std::max(0.0f, totalWidth - gutterWidth);
    const float totalHeight = avail.y;
    const auto& pal = theme::palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowPadding = 6.0f;
    const float rowGap = 3.0f;
    const float compactControlHeight = ImGui::GetFontSize() + 2.0f;
    const float labelHeight = static_cast<float>(theme::typeScale().label);
    const float headerHeight = std::max(labelHeight, compactControlHeight);
    const float minTrackHeight =
        rowPadding * 2.0f + headerHeight +
        compactControlHeight * 2.0f + rowGap * 2.0f;
    trackHeight = std::max(trackHeight, minTrackHeight);

    // Rows deliberately stop at their content boundary. The shell below them
    // is a canvas for future tracks, not a blank continuation of the gutter.
    theme::drawShellBackground(
        dl, theme::Rect{origin, ImVec2(origin.x + totalWidth,
                                      origin.y + totalHeight)});

    // Reserve the marker lane FIRST so its "Add marker track" button gets
    // interaction space before the timeline's big InvisibleButton claims the
    // whole region. The marker lane returns how much vertical space it used.
    const float markerLaneHeight = drawMarkerLane(edit, undo, transport, view,
        ImVec2(origin.x, origin.y + timelineHeight), totalWidth, gutterWidth,
        view.scrollSamples, view.samplesPerPixel);

    // Reserve the TRACK ROWS area as an invisible interaction widget. This
    // must NOT cover the ruler or marker lane, or it eats their clicks.
    // We account for the cursor advance the marker lane already did.
    const auto& tracks = edit.tracks();
    const auto& midiTracks = edit.midiTracks();
    // The Edit answers the solo question for every track type at once, so the
    // header dimming below matches exactly what GraphBuilder silences.
    const bool anySoloed = edit.anySoloed();
    // MIDI rows carry one extra control row (the instrument), so they are
    // taller than audio rows. Everything below the bands derives its position
    // from these two heights rather than assuming a single row size.
    const float midiTrackHeight = trackHeight + compactControlHeight + rowGap;
    const float audioRegionHeight =
        static_cast<float>(tracks.size()) * trackHeight;
    const float midiRegionHeight =
        static_cast<float>(midiTracks.size()) * midiTrackHeight;
    const float tracksRegionHeight = audioRegionHeight + midiRegionHeight;
    char areaBtn[32];
    std::snprintf(areaBtn, sizeof(areaBtn), "##timeline_area_%p", &view);
    // The InvisibleButton covers the clip lane (right of the gutter) so it
    // doesn't eat clicks meant for the gutter's gain/pan sliders.
    bool areaHovered = false;
    if (tracksRegionHeight > 0.0f) {
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + gutterWidth,
                   origin.y + timelineHeight + markerLaneHeight));
        ImGui::InvisibleButton(
            areaBtn, ImVec2(totalWidth - gutterWidth, tracksRegionHeight));
        areaHovered = ImGui::IsItemHovered();
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // Is the mouse over the ruler region? Check independently of areaHovered
    // (the InvisibleButton only covers the track-rows area, but the ruler is
    // above it and needs its own hover detection for click-to-seek).
    const bool windowHovered = ImGui::IsWindowHovered();
    // A drag is a hover that outlives the press. Plain IsWindowHovered goes
    // false the moment any item owns ActiveId — and the track area's own
    // InvisibleButton takes it on mouse-down — so anything that must keep
    // tracking the mouse *while held* has to ask with this flag instead. The
    // range selection below is exactly that: without it the selection was
    // dead on the press frame and never started at all.
    const bool windowHeldOrHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool rulerHovered = windowHovered &&
        mouse.y >= origin.y && mouse.y <= origin.y + timelineHeight &&
        mouse.x >= origin.x + gutterWidth;

    // Ruler background + ticks. Brighten slightly when hovered/clickable.
    dl->AddRectFilled(
        origin, ImVec2(origin.x + gutterWidth, origin.y + timelineHeight),
        C(pal.trackHeaderSurface));
    ImVec4 rulerBg = rulerHovered ? pal.surfaceBase : pal.rulerSurface;
    dl->AddRectFilled(ImVec2(origin.x + gutterWidth, origin.y),
                      ImVec2(origin.x + totalWidth, origin.y + timelineHeight),
                      C(rulerBg));
    dl->AddLine(ImVec2(origin.x + gutterWidth, origin.y + timelineHeight),
                ImVec2(origin.x + totalWidth, origin.y + timelineHeight),
                C(pal.border));
    // The session's rate, not a constant: the ruler, the grid and the snap
    // all have to agree with the document's own unit.
    const double sr = static_cast<double>(edit.sampleRate());
    // One grid drives both the ruler and the lane lines, so a line under a
    // clip is always the same division as the label above it.
    const GridStep grid = gridStepFor(view.tcMode, view.samplesPerPixel, sr);

    // Walks the visible grid at `step`. Majors and minors are walked
    // separately rather than one loop testing `sample % major`: at 29.97 fps a
    // whole-frame minor does not divide a whole-second major, and every major
    // tick would go missing.
    auto forEachGridLine = [&](int64_t step, auto&& fn) {
        if (step <= 0) return;
        const int64_t first = std::max<int64_t>(
            0, static_cast<int64_t>(std::floor(view.scrollSamples / step)) * step);
        for (int64_t sample = first; ; sample += step) {
            const double x = origin.x + gutterWidth +
                (sample - view.scrollSamples) / view.samplesPerPixel;
            if (x > origin.x + totalWidth) break;
            if (x >= origin.x + gutterWidth) fn(x, sample);
        }
    };

    ImFont* rulerFont = theme::fonts().monoSmall != nullptr
        ? theme::fonts().monoSmall : ImGui::GetFont();
    const float rulerFontSize = static_cast<float>(theme::typeScale().caption);
    dl->PushClipRect(ImVec2(origin.x + gutterWidth, origin.y),
                     ImVec2(origin.x + totalWidth, origin.y + timelineHeight), true);
    forEachGridLine(grid.minor, [&](double x, int64_t) {
        dl->AddLine(ImVec2(x, origin.y + 22.0f),
                    ImVec2(x, origin.y + timelineHeight), C(pal.border));
    });
    forEachGridLine(grid.major, [&](double x, int64_t sample) {
        dl->AddLine(ImVec2(x, origin.y + 15.0f),
                    ImVec2(x, origin.y + timelineHeight), C(pal.borderStrong));
        const std::string label = formatTimecode(sample, view.tcMode, sr);
        dl->AddText(rulerFont, rulerFontSize, ImVec2(x + 4.0f, origin.y + 2.0f),
                    C(pal.textMuted), label.c_str());
    });
    dl->PopClipRect();

    // The same divisions carried down into a lane. Called per row, after the
    // lane background and before its clips, so clips sit on the grid rather
    // than under it.
    const ImU32 gridMinorCol = C(ImVec4(pal.border.x, pal.border.y,
                                        pal.border.z, pal.border.w * 0.45f));
    const ImU32 gridMajorCol = C(ImVec4(pal.borderStrong.x, pal.borderStrong.y,
                                        pal.borderStrong.z,
                                        pal.borderStrong.w * 0.7f));
    auto drawLaneGrid = [&](float top, float height) {
        forEachGridLine(grid.minor, [&](double x, int64_t) {
            dl->AddLine(ImVec2(x, top), ImVec2(x, top + height), gridMinorCol);
        });
        forEachGridLine(grid.major, [&](double x, int64_t) {
            dl->AddLine(ImVec2(x, top), ImVec2(x, top + height), gridMajorCol);
        });
    };

    // ─── Add track ───────────────────────────────────────────────────────
    // A "+" in the gutter's ruler corner, directly above the topmost track
    // header. The list grows downward from the control that creates it, so
    // the affordance stays where the user left it instead of drifting further
    // down the window with every track added.
    constexpr float addTrackSize = 20.0f;
    const ImVec2 addTrackMin(
        origin.x + 10.0f,
        origin.y + (timelineHeight - addTrackSize) * 0.5f);
    const ImVec2 addTrackMax(addTrackMin.x + addTrackSize,
                             addTrackMin.y + addTrackSize);
    ImGui::SetCursorScreenPos(addTrackMin);
    ImGui::InvisibleButton("##addTrackTimeline",
                           ImVec2(addTrackSize, addTrackSize));
    const bool addTrackHovered =
        ImGui::IsItemHovered() || ImGui::IsItemActive();
    const bool addTrackRequested =
        ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (addTrackHovered) {
        ImGui::SetTooltip("Add track");
    }
    dl->AddRectFilled(addTrackMin, addTrackMax,
                      C(addTrackHovered ? pal.surfaceBase : pal.panel), 4.0f);
    dl->AddRect(addTrackMin, addTrackMax,
                C(addTrackHovered ? pal.accent : pal.borderStrong), 4.0f);
    const ImU32 plusCol = C(addTrackHovered ? pal.accentStrong : pal.textMuted);
    const ImVec2 plusCenter((addTrackMin.x + addTrackMax.x) * 0.5f,
                            (addTrackMin.y + addTrackMax.y) * 0.5f);
    constexpr float plusArm = 5.0f;
    dl->AddLine(ImVec2(plusCenter.x - plusArm, plusCenter.y),
                ImVec2(plusCenter.x + plusArm, plusCenter.y), plusCol, 1.5f);
    dl->AddLine(ImVec2(plusCenter.x, plusCenter.y - plusArm),
                ImVec2(plusCenter.x, plusCenter.y + plusArm), plusCol, 1.5f);

    // Marker lane was already drawn above (before the InvisibleButton). Reuse
    // its height to compute the track-row region.
    const float tracksTop = origin.y + timelineHeight + markerLaneHeight;
    const bool gutterHovered =
        windowHovered &&
        mouse.x >= origin.x && mouse.x <= origin.x + gutterWidth &&
        mouse.y >= tracksTop &&
        mouse.y <= tracksTop + tracksRegionHeight;

    // Where the MIDI band starts: immediately below the audio rows, so the two
    // read as one contiguous stack of tracks rather than two separate lists.
    const float midiTop = tracksTop + audioRegionHeight;

    // Row indices run across both bands — audio 0..n-1, then MIDI — the same
    // numbering selectedTrackIndex already uses, so a selection and a track
    // header selection can't disagree about which lane is which.
    auto rowAtY = [&](float y) -> int {
        if (y >= tracksTop && y < midiTop && trackHeight > 0.0f) {
            const int i = static_cast<int>((y - tracksTop) / trackHeight);
            if (i >= 0 && i < static_cast<int>(tracks.size())) return i;
        }
        if (y >= midiTop && y < midiTop + midiRegionHeight &&
            midiTrackHeight > 0.0f) {
            const int i = static_cast<int>((y - midiTop) / midiTrackHeight);
            if (i >= 0 && i < static_cast<int>(midiTracks.size())) {
                return static_cast<int>(tracks.size()) + i;
            }
        }
        return -1;
    };
    // The inverse, for drawing a lane-scoped selection. False when the row no
    // longer exists — a track deleted mid-selection.
    auto rowExtent = [&](int row, float& outY, float& outH) -> bool {
        if (row < 0) return false;
        if (row < static_cast<int>(tracks.size())) {
            outY = tracksTop + static_cast<float>(row) * trackHeight;
            outH = trackHeight;
            return true;
        }
        const int mi = row - static_cast<int>(tracks.size());
        if (mi < static_cast<int>(midiTracks.size())) {
            outY = midiTop + static_cast<float>(mi) * midiTrackHeight;
            outH = midiTrackHeight;
            return true;
        }
        return false;
    };

    GutterLayout gutter;
    gutter.dl = dl;
    gutter.origin = origin;
    gutter.gutterWidth = gutterWidth;
    gutter.rowPadding = rowPadding;
    gutter.rowGap = rowGap;
    gutter.headerHeight = headerHeight;
    gutter.labelHeight = labelHeight;
    gutter.compactControlHeight = compactControlHeight;
    gutter.mouse = mouse;
    gutter.gutterHovered = gutterHovered;

    // Tracks + clips.
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 1.0f));
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const auto& track = tracks[ti];
        float y = tracksTop + static_cast<float>(ti) * trackHeight;
        if (y > origin.y + totalHeight) break;

        bool selected = view.selectedTrackIndex == static_cast<int>(ti);
        // Header and lane get different surfaces rather than one fill across
        // the row. The lane is the darkest surface in the app so clips and
        // waveforms read as content sitting in a well, with the header column
        // stepping lighter as chrome above it — the arrangement PTXExtractor
        // uses. Alternating lane shading keeps adjacent tracks separable when
        // several are empty.
        const ImVec4 headerBg = selected ? pal.trackSelected : pal.trackHeaderSurface;
        const ImVec4 laneBg = ti % 2 == 0 ? pal.trackLaneSurface : pal.trackLaneAlt;
        dl->AddRectFilled(
            ImVec2(origin.x, y),
            ImVec2(origin.x + gutterWidth, y + trackHeight), C(headerBg));
        dl->AddRectFilled(
            ImVec2(origin.x + gutterWidth, y),
            ImVec2(origin.x + totalWidth, y + trackHeight), C(laneBg));
        // Selection reads on the lane too, or selecting a track would only
        // highlight the narrow header and be easy to miss at a glance.
        if (selected) {
            dl->AddRectFilled(
                ImVec2(origin.x + gutterWidth, y),
                ImVec2(origin.x + totalWidth, y + trackHeight),
                IM_COL32(static_cast<int>(pal.accent.x * 255),
                         static_cast<int>(pal.accent.y * 255),
                         static_cast<int>(pal.accent.z * 255), 18));
        }
        drawLaneGrid(y, trackHeight);
        // Track separator.
        dl->AddLine(ImVec2(origin.x, y + trackHeight),
                    ImVec2(origin.x + totalWidth, y + trackHeight),
                    C(pal.border));
        // Selection gets a dedicated accent edge. The smaller chip remains a
        // stable per-track identity when selection moves elsewhere.
        if (selected) {
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + 4, y + trackHeight),
                              C(pal.accent));
        }
        const ImVec4 trackColors[] = {
            pal.markerCue, pal.markerSection, pal.markerLoop,
            pal.markerPunch, pal.markerCd, pal.markerCustom
        };
        const ImVec4& trackColor =
            trackColors[ti % (sizeof(trackColors) / sizeof(trackColors[0]))];
        dl->AddRectFilled(
            ImVec2(origin.x + 10.0f, y + rowPadding),
            ImVec2(origin.x + 14.0f, y + rowPadding + headerHeight),
            C(trackColor), 2.0f);

        {
            auto& mutableTrack = const_cast<document::Track&>(track);
            drawTrackGutter(gutter, view, const_cast<document::Edit&>(edit), y,
                            static_cast<int>(ti),
                            TrackGutterFields{&mutableTrack.name,
                                              &mutableTrack.gain,
                                              &mutableTrack.pan,
                                              &mutableTrack.mute,
                                              &mutableTrack.solo},
                            selected);
        }

        // A soloed track elsewhere silences this one. Without a cue in the
        // header it looks like a working track that has simply stopped making
        // sound, which is a genuinely confusing state to debug by ear.
        if (!track.mute && !track.solo && anySoloed) {
            dl->AddRectFilled(
                ImVec2(origin.x, y),
                ImVec2(origin.x + gutterWidth, y + trackHeight),
                IM_COL32(20, 19, 18, 110));
        }

        // Click the gutter to select this track.
        if (gutterHovered &&
            mouse.y >= y && mouse.y <= y + trackHeight &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.selectedTrackIndex = static_cast<int>(ti);
        }

        for (const auto& clip : track.clips) {
            // A drag previews from view state instead of writing to the clip.
            // Mutating the document during the drag meant MoveClipCommand
            // snapshotted "the old position" after it had already been
            // overwritten, so undo restored the dragged-to position and the
            // move became permanent.
            const bool clipIsDragging =
                view.isDragging(TimelineViewState::DragKind::AudioClip) &&
                view.selectedClipId == clip.id;
            const int64_t drawStart =
                clipIsDragging ? view.dragPreviewStart : clip.timelineStart;
            double clipX = origin.x + gutterWidth +
                (drawStart - view.scrollSamples) / view.samplesPerPixel;
            double clipW = static_cast<double>(clip.length) / view.samplesPerPixel;
            if (clipW < 2) clipW = 2;
            if (clipX + clipW < origin.x + gutterWidth) continue;
            if (clipX > origin.x + totalWidth) continue;

            struct Rect { ImVec2 min, max; bool contains(const ImVec2& p) const {
                return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; } };
            Rect clipRect{ImVec2(clipX, y + 6),
                          ImVec2(clipX + clipW, y + trackHeight - 6)};
            bool isSel = view.selectedClipId == clip.id;
            // A distinct header gives a clip its identity without competing
            // with the waveform, which carries the edit-critical detail.
            ImVec4 body = pal.clipAudio;
            if (isSel) body = ImVec4(body.x + 0.1f, body.y + 0.08f, body.z + 0.05f, 1.0f);
            dl->AddRectFilled(clipRect.min, clipRect.max, C(body), 2.0f);
            dl->AddRect(clipRect.min, clipRect.max,
                        isSel ? C(pal.accent) : C(pal.clipAudioBorder), 2.0f);
            constexpr float clipHeaderHeight = 20.0f;
            const float headerBottom = std::min(clipRect.max.y - 4.0f,
                                                clipRect.min.y + clipHeaderHeight);
            const ImVec4 headerColor(
                pal.clipAudioBorder.x, pal.clipAudioBorder.y,
                pal.clipAudioBorder.z, isSel ? 0.40f : 0.30f);
            dl->AddRectFilled(clipRect.min,
                              ImVec2(clipRect.max.x, headerBottom),
                              C(headerColor), 2.0f,
                              ImDrawFlags_RoundCornersTop);
            if (clipW > 28.0) {
                ImFont* clipFont = theme::fonts().small != nullptr
                    ? theme::fonts().small : ImGui::GetFont();
                const document::AudioAsset* asset = edit.asset(clip.asset);
                const char* clipLabel = "Audio clip";
                if (asset != nullptr && !asset->path.empty()) {
                    const size_t lastSeparator = asset->path.find_last_of("/\\\\");
                    clipLabel = asset->path.c_str() +
                        (lastSeparator == std::string::npos ? 0 : lastSeparator + 1);
                }
                dl->PushClipRect(ImVec2(clipRect.min.x + 6.0f, clipRect.min.y),
                                 ImVec2(clipRect.max.x - 4.0f, headerBottom), true);
                dl->AddText(clipFont, static_cast<float>(theme::typeScale().caption),
                            ImVec2(clipRect.min.x + 6.0f, clipRect.min.y + 3.0f),
                            C(pal.primaryText), clipLabel);
                dl->PopClipRect();
            }

            // Waveform (lighter than the body so it reads).
            auto bufIt = assetBuffers.find(clip.asset.sha256);
            if (bufIt != assetBuffers.end()) {
                const int spp = std::max(1, static_cast<int>(view.samplesPerPixel));
                const auto& level = peaks.get(clip.asset.sha256, bufIt->second, spp);
                const float waveformTop = headerBottom + 4.0f;
                const float waveformBottom = clipRect.max.y - 4.0f;
                const float waveformMidY = (waveformTop + waveformBottom) * 0.5f;
                const float waveformHalfH = std::max(2.0f,
                    (waveformBottom - waveformTop) * 0.5f);
                const float waveformScale = level.displayScale;
                int64_t numDraw = static_cast<int64_t>(clipW);
                ImU32 waveCol = C(pal.clipAudioBorder);
                for (int64_t px = 0; px < numDraw; ++px) {
                    const int64_t sourceStart = clip.sourceOffset +
                        static_cast<int64_t>(std::floor(px * view.samplesPerPixel));
                    const int64_t sourceEnd = clip.sourceOffset +
                        static_cast<int64_t>(std::ceil((px + 1) * view.samplesPerPixel));
                    const int64_t firstBucket = sourceStart / level.bucketSize;
                    const int64_t lastBucket = std::max(firstBucket,
                        (sourceEnd - 1) / level.bucketSize);
                    float peakMin = 1.0f;
                    float peakMax = -1.0f;
                    for (int64_t bucket = firstBucket; bucket <= lastBucket; ++bucket) {
                        if (bucket < 0 ||
                            bucket >= static_cast<int64_t>(level.buckets.size())) continue;
                        peakMin = std::min(peakMin, level.buckets[bucket].min);
                        peakMax = std::max(peakMax, level.buckets[bucket].max);
                    }
                    if (peakMin > peakMax) continue;
                    peakMin = std::clamp(peakMin * waveformScale, -1.0f, 1.0f);
                    peakMax = std::clamp(peakMax * waveformScale, -1.0f, 1.0f);
                    float pxX = static_cast<float>(clipX + px);
                    dl->AddLine(ImVec2(pxX, waveformMidY - peakMax * waveformHalfH),
                                ImVec2(pxX, waveformMidY - peakMin * waveformHalfH), waveCol);
                }
            }

            // Interaction: drag to move.
            const bool clipHovered = clipRect.contains(mouse);
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                view.dragClipOriginalStart = clip.timelineStart;
                // Seed the preview so a click without movement draws in place.
                view.dragPreviewStart = clip.timelineStart;
                view.dragOriginalTrackId = track.id;
                view.dragKind = TimelineViewState::DragKind::AudioClip;
            }
            // Right-click: context menu (split, delete).
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                ImGui::OpenPopup("##clip_ctx");
            }
        }
    }

    // ─── MIDI rows ──────────────────────────────────────────────────────────
    // A contiguous band directly below the audio rows. Same row anatomy, same
    // gutter, one extra control row for the instrument — a MIDI track differs
    // from an audio track in what feeds it, not in how it is arranged.
    for (size_t mi = 0; mi < midiTracks.size(); ++mi) {
        const auto& track = midiTracks[mi];
        const float y = midiTop + static_cast<float>(mi) * midiTrackHeight;
        if (y > origin.y + totalHeight) break;
        // Ids continue past the audio tracks so ImGui state and the single
        // inline-rename slot can't collide between the two bands.
        const int uid = static_cast<int>(tracks.size() + mi);
        const bool selected = view.selectedTrackIndex == uid;

        const ImVec4 headerBg = selected ? pal.trackSelected : pal.trackHeaderSurface;
        const ImVec4 laneBg = (tracks.size() + mi) % 2 == 0
            ? pal.trackLaneSurface : pal.trackLaneAlt;
        dl->AddRectFilled(ImVec2(origin.x, y),
                          ImVec2(origin.x + gutterWidth, y + midiTrackHeight),
                          C(headerBg));
        dl->AddRectFilled(ImVec2(origin.x + gutterWidth, y),
                          ImVec2(origin.x + totalWidth, y + midiTrackHeight),
                          C(laneBg));
        if (selected) {
            dl->AddRectFilled(
                ImVec2(origin.x + gutterWidth, y),
                ImVec2(origin.x + totalWidth, y + midiTrackHeight),
                IM_COL32(static_cast<int>(pal.accent.x * 255),
                         static_cast<int>(pal.accent.y * 255),
                         static_cast<int>(pal.accent.z * 255), 18));
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + 4, y + midiTrackHeight),
                              C(pal.accent));
        }
        drawLaneGrid(y, midiTrackHeight);
        dl->AddLine(ImVec2(origin.x, y + midiTrackHeight),
                    ImVec2(origin.x + totalWidth, y + midiTrackHeight),
                    C(pal.border));
        // The identity chip uses the MIDI clip colour, so the band is
        // recognisable as MIDI at a glance without reading any labels.
        dl->AddRectFilled(ImVec2(origin.x + 10.0f, y + rowPadding),
                          ImVec2(origin.x + 14.0f, y + rowPadding + headerHeight),
                          C(pal.clipMidiBorder), 2.0f);

        auto& mutableTrack = const_cast<document::MidiTrack&>(track);
        const float instrumentY = drawTrackGutter(
            gutter, view, const_cast<document::Edit&>(edit), y, uid,
            TrackGutterFields{&mutableTrack.name, &mutableTrack.gain,
                              &mutableTrack.pan, &mutableTrack.mute,
                              &mutableTrack.solo},
            selected);

        // Instrument row. The whole strip is one button: clicking it opens the
        // picker when empty and the plugin's own editor when filled, which is
        // the only thing you ever want to do with the name of a synth.
        {
            ImGui::PushID(uid + 9000);
            const float instX = origin.x + 12.0f;
            const float instW = gutterWidth - 24.0f;
            ImGui::SetCursorScreenPos(ImVec2(instX, instrumentY));
            const bool pressed =
                ImGui::InvisibleButton("##instrument",
                                       ImVec2(instW, compactControlHeight));
            const bool hovered = ImGui::IsItemHovered();
            const bool hasInstrument = !track.instrument.uidString.empty();
            if (pressed) {
                if (hasInstrument) {
                    view.requestPluginEditorSlotId = track.instrument.id;
                } else {
                    view.requestPicker =
                        TimelineViewState::PluginPicker::MidiInstrument;
                    view.requestPickerTrackId = track.id;
                }
            }
            const ImVec2 iMin(instX, instrumentY);
            const ImVec2 iMax(instX + instW, instrumentY + compactControlHeight);
            dl->AddRectFilled(iMin, iMax,
                              hovered ? C(pal.surfaceStrong)
                                      : C(pal.trackControlInactive), 3.0f);
            dl->AddRect(iMin, iMax,
                        hasInstrument ? C(pal.clipMidiBorder) : C(pal.border), 3.0f);
            dl->PushClipRect(ImVec2(iMin.x + 5.0f, iMin.y),
                             ImVec2(iMax.x - 4.0f, iMax.y), true);
            dl->AddText(ImVec2(iMin.x + 6.0f, iMin.y + 1.0f),
                        hasInstrument ? C(pal.text) : C(pal.textSubtle),
                        hasInstrument ? track.instrument.name.c_str()
                                      : "Set Instrument…");
            dl->PopClipRect();
            if (hovered) {
                ImGui::SetTooltip("%s", hasInstrument
                    ? "Open the instrument's editor"
                    : "Choose an instrument for this track");
            }
            ImGui::PopID();
        }

        if (!track.mute && !track.solo && anySoloed) {
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + gutterWidth, y + midiTrackHeight),
                              IM_COL32(20, 19, 18, 110));
        }

        if (gutterHovered && mouse.y >= y && mouse.y <= y + midiTrackHeight &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.selectedTrackIndex = uid;
        }

        for (const auto& clip : track.clips) {
            const bool clipIsDragging =
                view.isDragging(TimelineViewState::DragKind::MidiClip) &&
                view.selectedClipId == clip.id;
            const int64_t drawStart =
                clipIsDragging ? view.dragPreviewStart : clip.timelineStart;
            double clipX = origin.x + gutterWidth +
                (drawStart - view.scrollSamples) / view.samplesPerPixel;
            double clipW = static_cast<double>(clip.length) / view.samplesPerPixel;
            if (clipW < 2) clipW = 2;
            if (clipX + clipW < origin.x + gutterWidth) continue;
            if (clipX > origin.x + totalWidth) continue;

            const ImVec2 clipMin(static_cast<float>(clipX), y + 6.0f);
            const ImVec2 clipMax(static_cast<float>(clipX + clipW),
                                 y + midiTrackHeight - 6.0f);
            const bool isSel = view.selectedClipId == clip.id;
            ImVec4 body = pal.clipMidi;
            if (isSel) body = ImVec4(body.x + 0.1f, body.y + 0.08f, body.z + 0.05f, 1.0f);
            dl->AddRectFilled(clipMin, clipMax, C(body), 2.0f);
            dl->AddRect(clipMin, clipMax,
                        isSel ? C(pal.accent) : C(pal.clipMidiBorder), 2.0f);
            constexpr float clipHeaderHeight = 20.0f;
            const float headerBottom =
                std::min(clipMax.y - 4.0f, clipMin.y + clipHeaderHeight);
            const ImVec4 headerColor(pal.clipMidiBorder.x, pal.clipMidiBorder.y,
                                     pal.clipMidiBorder.z, isSel ? 0.40f : 0.30f);
            dl->AddRectFilled(clipMin, ImVec2(clipMax.x, headerBottom),
                              C(headerColor), 2.0f, ImDrawFlags_RoundCornersTop);
            if (clipW > 28.0) {
                ImFont* clipFont = theme::fonts().small != nullptr
                    ? theme::fonts().small : ImGui::GetFont();
                dl->PushClipRect(ImVec2(clipMin.x + 6.0f, clipMin.y),
                                 ImVec2(clipMax.x - 4.0f, headerBottom), true);
                dl->AddText(clipFont, static_cast<float>(theme::typeScale().caption),
                            ImVec2(clipMin.x + 6.0f, clipMin.y + 3.0f),
                            C(pal.primaryText),
                            clip.name.empty() ? "MIDI clip" : clip.name.c_str());
                dl->PopClipRect();
            }

            // Note blobs. Pitch is normalised over the clip's own range with a
            // one-octave floor, so a two-note bass part doesn't spread across
            // the whole row and read like a melody — but a wide part still
            // fills the space it has.
            const float noteTop = headerBottom + 3.0f;
            const float noteBottom = clipMax.y - 3.0f;
            if (noteBottom - noteTop >= 6.0f && !clip.notes.empty()) {
                uint8_t lo = 127, hi = 0;
                for (const auto& n : clip.notes) {
                    lo = std::min(lo, n.pitch);
                    hi = std::max(hi, n.pitch);
                }
                const float span = std::max(12.0f, static_cast<float>(hi - lo) + 1.0f);
                const float noteH = std::max(2.0f, (noteBottom - noteTop) / span);
                const ImU32 noteCol = C(pal.midiNote);
                // Only the notes inside the clip's trim window sound, and only
                // the ones on screen are worth drawing — a dense part is tens
                // of thousands of notes and this runs every frame.
                const int64_t winStart = clip.sourceOffset;
                const int64_t winEnd = clip.sourceOffset + clip.length;
                for (const auto& n : clip.notes) {
                    if (n.startSample < winStart || n.startSample >= winEnd) continue;
                    const double nx = clipX +
                        (n.startSample - winStart) / view.samplesPerPixel;
                    if (nx > clipMax.x) break;   // notes are sorted by start
                    double nw = n.lengthSamples / view.samplesPerPixel;
                    if (nw < 2.0) nw = 2.0;
                    if (nx + nw < clipMin.x) continue;
                    const float ny = noteBottom -
                        (static_cast<float>(n.pitch - lo) + 1.0f) / span *
                        (noteBottom - noteTop);
                    dl->AddRectFilled(
                        ImVec2(std::max(static_cast<float>(nx), clipMin.x + 1.0f), ny),
                        ImVec2(std::min(static_cast<float>(nx + nw), clipMax.x - 1.0f),
                               ny + noteH),
                        noteCol);
                }
            }

            const bool clipHovered =
                mouse.x >= clipMin.x && mouse.x <= clipMax.x &&
                mouse.y >= clipMin.y && mouse.y <= clipMax.y;
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = uid;
                view.dragClipOriginalStart = clip.timelineStart;
                view.dragPreviewStart = clip.timelineStart;
                view.dragOriginalTrackId = track.id;
                view.dragKind = TimelineViewState::DragKind::MidiClip;
            }
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = uid;
                view.contextMidiTrackId = track.id;
                ImGui::OpenPopup("##midi_clip_ctx");
            }
        }
    }
    ImGui::PopStyleVar();

    // Picture is an opt-in: a session that never imported video has no lane,
    // so the timeline shows only what the project actually contains.
    const bool hasVideo = !edit.videoTracks().empty();
    const float videoLaneY = tracksTop + tracksRegionHeight;
    // The spare canvas is deliberately the fastest way to create another
    // track: the GLFW drop callback imports each WAV into a fresh lane.
    const float videoLaneHeight = hasVideo ? 40.0f : 0.0f;
    const float canvasTop = videoLaneY + videoLaneHeight;
    const float canvasHeight = origin.y + totalHeight - canvasTop;
    if (canvasHeight >= 72.0f) {
        const char* canvasLabel = "Drop WAV files here to create tracks";
        const ImVec2 labelSize = ImGui::CalcTextSize(canvasLabel);
        dl->AddText(ImVec2(origin.x + (totalWidth - labelSize.x) * 0.5f,
                           canvasTop + (canvasHeight - labelSize.y) * 0.5f),
                    C(pal.textSubtle), canvasLabel);
    }
    const bool canvasHovered =
        windowHeldOrHovered &&
        mouse.x >= origin.x + gutterWidth &&
        mouse.x <= origin.x + totalWidth &&
        mouse.y >= tracksTop &&
        mouse.y <= origin.y + totalHeight &&
        !(hasVideo && mouse.y >= videoLaneY &&
          mouse.y <= videoLaneY + videoLaneHeight);

    // Clip context menu (right-click on a clip).
    if (ImGui::BeginPopup("##clip_ctx")) {
        if (!view.selectedClipId.empty() && view.selectedTrackIndex >= 0) {
            const auto& tracks = edit.tracks();
            if (view.selectedTrackIndex < static_cast<int>(tracks.size())) {
                const auto& trk = tracks[view.selectedTrackIndex];
                int64_t playhead = transport.position();
                if (ImGui::MenuItem("Split at Playhead")) {
                    undo.execute(std::make_unique<editing::SplitClipCommand>(
                        trk.id, view.selectedClipId, playhead));
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Clip")) {
                    undo.execute(std::make_unique<editing::RemoveClipCommand>(
                        trk.id, view.selectedClipId));
                    view.selectedClipId.clear();
                }
            }
        }
        ImGui::EndPopup();
    }

    // MIDI clip context menu.
    if (ImGui::BeginPopup("##midi_clip_ctx")) {
        if (!view.selectedClipId.empty() && !view.contextMidiTrackId.empty()) {
            if (ImGui::MenuItem("Split at Playhead")) {
                undo.execute(std::make_unique<editing::SplitMidiClipCommand>(
                    view.contextMidiTrackId, view.selectedClipId,
                    transport.position()));
            }
            if (ImGui::MenuItem("Duplicate")) {
                undo.execute(std::make_unique<editing::DuplicateMidiClipCommand>(
                    view.contextMidiTrackId, view.selectedClipId));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Clip")) {
                undo.execute(std::make_unique<editing::RemoveMidiClipCommand>(
                    view.contextMidiTrackId, view.selectedClipId));
                view.selectedClipId.clear();
                view.contextMidiTrackId.clear();
            }
        }
        ImGui::EndPopup();
    }

    // Snap helper: if snapToMarkers is on, snap a sample position to the
    // nearest marker within a threshold (8px worth of samples).
    auto snapPosition = [&](int64_t pos) -> int64_t {
        if (!view.snapToMarkers) return pos;
        int64_t threshold = static_cast<int64_t>(8.0 * view.samplesPerPixel);
        int64_t best = pos;
        int64_t bestDist = threshold;
        for (const auto& mt : edit.markerTracks()) {
            if (!mt.visible) continue;
            for (const auto& m : mt.markers) {
                if (m.posMode != document::MarkerPosMode::Sample) continue;
                int64_t d = std::abs(m.position - pos);
                if (d < bestDist) { bestDist = d; best = m.position; }
            }
        }
        return best;
    };

    // Handle drag delta for the active drag (committed on release).
    const bool draggingAudioClip =
        view.isDragging(TimelineViewState::DragKind::AudioClip);
    const bool draggingMidiClip =
        view.isDragging(TimelineViewState::DragKind::MidiClip);
    if ((draggingAudioClip || draggingMidiClip) && !view.selectedClipId.empty()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.dragStartMouseX = mouse.x;
            view.dragStartMouseY = mouse.y;
        }
        // Figure out which track the mouse is currently over (by Y). A clip
        // stays in its own band: an audio clip has no instrument to play it on
        // a MIDI track, and a note sequence has no waveform for an audio one.
        int targetTrackIndex = -1;
        if (draggingMidiClip) {
            if (mouse.y >= midiTop && midiTrackHeight > 0.0f) {
                targetTrackIndex =
                    static_cast<int>((mouse.y - midiTop) / midiTrackHeight);
                if (targetTrackIndex < 0 ||
                    targetTrackIndex >= static_cast<int>(midiTracks.size())) {
                    targetTrackIndex = -1;
                }
            }
        } else if (mouse.y >= tracksTop && mouse.y < midiTop) {
            targetTrackIndex = static_cast<int>((mouse.y - tracksTop) / trackHeight);
            if (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int>(tracks.size())) {
                targetTrackIndex = -1;
            }
        }

        // Preview only — the document is not touched until mouse-up, so
        // MoveClipCommand can snapshot a genuine "before" for undo, and the
        // engine never sees a position the user hasn't committed to.
        const int64_t dxPx =
            static_cast<int64_t>(mouse.x - view.dragStartMouseX);
        int64_t dxSamples = static_cast<int64_t>(dxPx * view.samplesPerPixel);
        {
            int64_t raw = std::max<int64_t>(0, view.dragClipOriginalStart + dxSamples);
            view.dragPreviewStart = snapPosition(raw);
        }
        // Visual hint: highlight the track being hovered during drag.
        if (targetTrackIndex >= 0) {
            const float rowHeight = draggingMidiClip ? midiTrackHeight : trackHeight;
            const float bandTop = draggingMidiClip ? midiTop : tracksTop;
            const float ty = bandTop + static_cast<float>(targetTrackIndex) * rowHeight;
            dl->AddRectFilled(ImVec2(origin.x + gutterWidth, ty),
                              ImVec2(origin.x + totalWidth, ty + rowHeight),
                              C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.12f)));
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Commit via command. If the mouse ended on a different track, move it.
            std::string targetTrackId = view.dragOriginalTrackId;
            if (targetTrackIndex >= 0) {
                targetTrackId = draggingMidiClip ? midiTracks[targetTrackIndex].id
                                                 : tracks[targetTrackIndex].id;
            }
            const std::string newTrackArg =
                (targetTrackId != view.dragOriginalTrackId) ? targetTrackId : "";
            // Skip a no-op drag so a stray click doesn't push an undo entry
            // that appears to do nothing when replayed.
            const bool moved = view.dragPreviewStart != view.dragClipOriginalStart ||
                               !newTrackArg.empty();
            if (moved) {
                if (draggingMidiClip) {
                    undo.execute(std::make_unique<editing::MoveMidiClipCommand>(
                        view.dragOriginalTrackId, view.selectedClipId,
                        view.dragPreviewStart, newTrackArg));
                } else {
                    undo.execute(std::make_unique<editing::MoveClipCommand>(
                        view.dragOriginalTrackId, view.selectedClipId,
                        view.dragPreviewStart, newTrackArg));
                }
            }
            view.dragKind = TimelineViewState::DragKind::None;
            view.selectedClipId.clear();
            view.dragOriginalTrackId.clear();
        }
    }

    // --- Seek by clicking the ruler or empty track area --------------------
    // The ruler always seeks (even if a clip visually overlaps below it) —
    // that's the standard DAW affordance. The track area seeks only when the
    // click isn't on a clip (so clip-drag still works there).
    auto seekToMouseX = [&]() {
        double localX = mouse.x - (origin.x + gutterWidth);
        int64_t seekTo = static_cast<int64_t>(
            view.scrollSamples + std::max(0.0, localX) * view.samplesPerPixel);
        transport.seek(snapPosition(seekTo));
    };

    // Sample position under the mouse, clamped to the start of the timeline.
    auto sampleAtMouseX = [&]() -> int64_t {
        const double localX = mouse.x - (origin.x + gutterWidth);
        return static_cast<int64_t>(
            view.scrollSamples + std::max(0.0, localX) * view.samplesPerPixel);
    };

    // Selection edges land on the current format's own divisions — whole
    // frames in timecode, whole beat subdivisions in bars|beats — so a range
    // is always a span the format can name, and the same one the grid draws.
    const int64_t snapStep =
        snapStepFor(view.tcMode, view.samplesPerPixel, sr);
    auto snapToFormat = [&](int64_t sample) -> int64_t {
        if (snapStep <= 1) return sample;
        const int64_t half = snapStep / 2;
        return ((sample + half) / snapStep) * snapStep;
    };
    auto selectionSampleAtMouseX = [&]() -> int64_t {
        return snapToFormat(sampleAtMouseX());
    };

    // Ruler: the press seeks, and dragging from it selects the range across
    // every track — the one gesture that reaches all lanes at once. (This
    // replaces drag-to-scrub: the playhead can follow the mouse or the drag
    // can mark a range, not both.)
    if (rulerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        seekToMouseX();
        view.selectionPressSample = sampleAtMouseX();
        const int64_t s = selectionSampleAtMouseX();
        view.selectionStart = s;
        view.selectionEnd = s;
        view.selectionRow = -1;          // -1 = all tracks
        view.isSelecting = true;
        view.hasSelection = true;
    }

    // Track area: click empty space seeks (but not on clips — those drag).
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !view.isDragging()) {
        bool hitClip = false;
        auto spansMouse = [&](int64_t start, int64_t length) {
            const double x = origin.x + gutterWidth +
                (start - view.scrollSamples) / view.samplesPerPixel;
            const double w = static_cast<double>(length) / view.samplesPerPixel;
            return mouse.x >= x && mouse.x <= x + w;
        };
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (spansMouse(clip.timelineStart, clip.length)) { hitClip = true; break; }
            }
            if (hitClip) break;
        }
        for (const auto& track : midiTracks) {
            if (hitClip) break;
            for (const auto& clip : track.clips) {
                if (spansMouse(clip.timelineStart, clip.length)) { hitClip = true; break; }
            }
        }
        if (!hitClip) {
            // Start a selection drag (instead of just seeking). The row under
            // the press owns it: a range dragged in one lane stays in that
            // lane no matter how far the pointer wanders vertically.
            const int row = rowAtY(mouse.y);
            view.selectionPressSample = sampleAtMouseX();
            const int64_t s = selectionSampleAtMouseX();
            view.selectionStart = s;
            view.selectionEnd = s;
            view.selectionRow = row;
            view.isSelecting = true;
            // Below the last track there is no lane to select in, so the press
            // is a plain seek and nothing highlights.
            view.hasSelection = row >= 0;
            if (row >= 0) view.selectedTrackIndex = row;
        }
    }

    // Update selection end while dragging. Deliberately not gated on hover:
    // a drag that runs off the lane — up onto the ruler, or past the window
    // edge — should keep extending rather than freeze where it left.
    if (view.isSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        view.selectionEnd = selectionSampleAtMouseX();
    }
    // End selection on mouse release.
    if (view.isSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        view.isSelecting = false;
        // If the selection is tiny (just a click, not a drag), treat as seek + clear selection.
        if (std::abs(view.selectionEnd - view.selectionStart) <
            static_cast<int64_t>(4 * view.samplesPerPixel)) {
            // Not a drag after all: the cursor goes where the click landed,
            // not to the division the selection edge would have snapped to.
            transport.seek(view.selectionPressSample);
            view.hasSelection = false;
        } else if (view.hasSelection) {
            // A finished selection puts the cursor at its head, so play
            // starts from the top of the range — and, with loop on, so does
            // the loop. The head is the lower edge whichever direction the
            // drag ran: dragging right to left selects the same range, and
            // dropping the cursor at the pointer would leave it at the tail.
            transport.seek(std::min(view.selectionStart, view.selectionEnd));
        }
    }
    // Clear selection on Escape or clicking elsewhere.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        view.hasSelection = false;
    }

    // A selection dragged in a lane is drawn over that lane alone; only the
    // ruler's spans the arrangement. A row that has since been deleted takes
    // its selection with it rather than highlighting whichever track slid into
    // its place.
    float selTop = origin.y + timelineHeight;
    float selBottom = origin.y + totalHeight;
    if (view.hasSelection && view.selectionRow >= 0) {
        float rowY = 0.0f, rowH = 0.0f;
        if (rowExtent(view.selectionRow, rowY, rowH)) {
            selTop = rowY;
            selBottom = rowY + rowH;
        } else {
            view.hasSelection = false;
        }
    }

    // Draw selection region (translucent highlight).
    if (view.hasSelection) {
        int64_t selMin = std::min(view.selectionStart, view.selectionEnd);
        int64_t selMax = std::max(view.selectionStart, view.selectionEnd);
        double selX1 = origin.x + gutterWidth + (selMin - view.scrollSamples) / view.samplesPerPixel;
        double selX2 = origin.x + gutterWidth + (selMax - view.scrollSamples) / view.samplesPerPixel;
        if (selX2 > selX1) {
            dl->AddRectFilled(
                ImVec2(selX1, selTop), ImVec2(selX2, selBottom),
                C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.15f)));
            dl->AddRect(
                ImVec2(selX1, selTop), ImVec2(selX2, selBottom),
                C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.5f)));
            // Time readout at the top of the selection.
            double durSec = (selMax - selMin) / sr;
            char selLabel[32];
            std::snprintf(selLabel, sizeof(selLabel), "%.2fs", durSec);
            dl->AddText(ImVec2((selX1 + selX2) / 2 - 15, selTop + 2),
                        C(pal.accent), selLabel);
        }
    }

    // A shaded rail survives both the clip body and the shell, while the thin
    // bright core preserves the precise position needed for edit decisions.
    int64_t pos = transport.position();
    double playheadX = origin.x + gutterWidth +
        (pos - view.scrollSamples) / view.samplesPerPixel;
    if (playheadX >= origin.x + gutterWidth && playheadX <= origin.x + totalWidth) {
        dl->AddLine(ImVec2(playheadX, origin.y + timelineHeight),
                    ImVec2(playheadX, origin.y + totalHeight),
                    C(pal.accentDeep), 3.0f);
        dl->AddLine(ImVec2(playheadX, origin.y + timelineHeight),
                    ImVec2(playheadX, origin.y + totalHeight),
                    C(pal.accentStrong), 1.0f);
        // The head shrinks with the line. A heavy flag over a hairline reads
        // as a marker sitting near the playhead rather than as its top.
        dl->AddRectFilled(
            ImVec2(playheadX - 3.0f, origin.y + timelineHeight - 7.0f),
            ImVec2(playheadX + 3.0f, origin.y + timelineHeight - 3.0f),
            C(pal.accentStrong), 1.5f);
        dl->AddTriangleFilled(
            ImVec2(playheadX - 5, origin.y + timelineHeight - 3),
            ImVec2(playheadX + 5, origin.y + timelineHeight - 3),
            ImVec2(playheadX, origin.y + timelineHeight + 3), C(pal.accentStrong));
    }

    // Video lane — a strip at the bottom of the timeline showing video clips.
    // Placed below the track rows, above the scroll/zoom handler. Absent
    // entirely until picture is imported.
    if (hasVideo) {
        const float drawnLaneHeight = drawVideoLane(
            edit, transport, view, ImVec2(origin.x, videoLaneY),
            totalWidth, gutterWidth, view.scrollSamples,
            view.samplesPerPixel);
        // Register the absolute-drawn lane with ImGui's content extent so a
        // long track list remains vertically scrollable down to its last lane.
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x, videoLaneY + drawnLaneHeight));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }

    // Scroll / zoom.
    double wheel = ImGui::GetIO().MouseWheel;
    if (canvasHovered && wheel != 0.0) {
        // Cmd or Ctrl. On macOS ImGui reports Command as KeySuper, and the
        // system claims Ctrl+scroll for its own screen zoom before the app
        // sees it — testing KeyCtrl alone left wheel zoom unreachable there.
        if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {
            // Scale the wheel step with the current zoom: a fixed 10 spp per
            // notch takes hundreds of turns to cross the range now that the
            // ceiling is 1,000,000.
            const double zoomStep = std::max(10.0, view.samplesPerPixel * 0.1);
            zoomAroundSample(view, view.samplesPerPixel - wheel * zoomStep,
                             transport.position());
        } else {
            view.scrollSamples = std::max(0.0, view.scrollSamples - wheel * view.samplesPerPixel * 20.0);
        }
    }

    // Adding reallocates the track vector, so it happens only after every
    // reference used for drawing and interaction has gone out of scope.
    if (addTrackRequested) {
        undo.execute(std::make_unique<editing::AddTrackCommand>("Track"));
        view.selectedTrackIndex =
            static_cast<int>(edit.tracks().size()) - 1;
    }
}

// ─── Marker lane ────────────────────────────────────────────────────────────

namespace {

// Default color per marker kind (used when Marker::color is empty).
ImU32 markerColor(document::MarkerKind k) {
    const auto& pal = theme::palette();
    switch (k) {
        case document::MarkerKind::Cue:     return C(pal.markerCue);
        case document::MarkerKind::Section: return C(pal.markerSection);
        case document::MarkerKind::Loop:    return C(pal.markerLoop);
        case document::MarkerKind::Punch:   return C(pal.markerPunch);
        case document::MarkerKind::CD:      return C(pal.markerCd);
        default:                            return C(pal.markerCustom);
    }
}

ImU32 parseHexColor(const std::string& hex, ImU32 fallback) {
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int r = nib(hex[1]) * 16 + nib(hex[2]);
    int g = nib(hex[3]) * 16 + nib(hex[4]);
    int b = nib(hex[5]) * 16 + nib(hex[6]);
    if (r < 0 || g < 0 || b < 0) return fallback;
    return IM_COL32(r, g, b, 255);
}

const char* markerKindLabel(document::MarkerKind k) {
    switch (k) {
        case document::MarkerKind::Cue:     return "Cue";
        case document::MarkerKind::Section: return "Section";
        case document::MarkerKind::Loop:    return "Loop";
        case document::MarkerKind::Punch:   return "Punch";
        case document::MarkerKind::CD:      return "CD";
        default:                            return "Custom";
    }
}

} // namespace

float drawMarkerLane(const document::Edit& edit,
                     editing::UndoStack& undo,
                     engine::Transport& transport,
                     TimelineViewState& view,
                     ImVec2 origin, float totalWidth, float gutterWidth,
                     double scrollSamples, double samplesPerPixel) {
    using namespace dave::document;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float laneHeight = 28.0f;
    const auto& pal = theme::palette();

    // Background — distinct from the ruler above and tracks below.
    dl->AddRectFilled(origin, ImVec2(origin.x + totalWidth, origin.y + laneHeight),
                      C(pal.panel));
    dl->AddLine(ImVec2(origin.x, origin.y + laneHeight),
                ImVec2(origin.x + totalWidth, origin.y + laneHeight),
                C(pal.border));
    // "Markers" label in the gutter.
    dl->AddText(ImVec2(origin.x + 10, origin.y + 6), C(pal.textMuted), "Markers");

    // Add-marker "+", right-justified in the gutter. The lane's own gestures
    // (double-click, right-click) drop a marker wherever the pointer is, so
    // the button takes the other position worth having: the playhead.
    constexpr float addMarkerSize = 18.0f;
    const ImVec2 addMarkerMin(
        origin.x + gutterWidth - 10.0f - addMarkerSize,
        origin.y + (laneHeight - addMarkerSize) * 0.5f);
    const ImVec2 addMarkerMax(addMarkerMin.x + addMarkerSize,
                              addMarkerMin.y + addMarkerSize);
    ImGui::SetCursorScreenPos(addMarkerMin);
    ImGui::InvisibleButton("##addMarkerTimeline",
                           ImVec2(addMarkerSize, addMarkerSize));
    const bool addMarkerHovered =
        ImGui::IsItemHovered() || ImGui::IsItemActive();
    const bool addMarkerRequested =
        ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (addMarkerHovered) {
        ImGui::SetTooltip("Add marker at playhead");
    }
    dl->AddRectFilled(addMarkerMin, addMarkerMax,
                      C(addMarkerHovered ? pal.surfaceBase : pal.bgElevated),
                      4.0f);
    dl->AddRect(addMarkerMin, addMarkerMax,
                C(addMarkerHovered ? pal.accent : pal.borderStrong), 4.0f);
    const ImU32 markerPlusCol =
        C(addMarkerHovered ? pal.accentStrong : pal.textMuted);
    const ImVec2 markerPlusCenter((addMarkerMin.x + addMarkerMax.x) * 0.5f,
                                  (addMarkerMin.y + addMarkerMax.y) * 0.5f);
    constexpr float markerPlusArm = 4.0f;
    dl->AddLine(ImVec2(markerPlusCenter.x - markerPlusArm, markerPlusCenter.y),
                ImVec2(markerPlusCenter.x + markerPlusArm, markerPlusCenter.y),
                markerPlusCol, 1.5f);
    dl->AddLine(ImVec2(markerPlusCenter.x, markerPlusCenter.y - markerPlusArm),
                ImVec2(markerPlusCenter.x, markerPlusCenter.y + markerPlusArm),
                markerPlusCol, 1.5f);
    if (addMarkerRequested) {
        // A session with no marker track yet gets one here rather than
        // refusing the click — the button means "put a marker down", and
        // where it lives is bookkeeping.
        if (edit.markerTracks().empty()) {
            undo.execute(
                std::make_unique<editing::AddMarkerTrackCommand>("Markers"));
        }
        if (!edit.markerTracks().empty()) {
            Marker marker;
            marker.name = "Marker";
            marker.position = transport.position();
            undo.execute(std::make_unique<editing::AddMarkerCommand>(
                edit.markerTracks().front().id, marker));
        }
    }

    // If no marker track exists, prompt with a hint and offer creation.
    if (edit.markerTracks().empty()) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + gutterWidth + 8, origin.y + 4));
        if (ImGui::SmallButton("Add marker track")) {
            undo.execute(std::make_unique<editing::AddMarkerTrackCommand>("Markers"));
        }
        return laneHeight;
    }

    // Flatten visible markers across all visible marker tracks. For RB-4 they
    // all render in one lane; per-track stacking comes later.
    struct MarkedItem { const Marker* m; const MarkerTrack* t; };
    std::vector<MarkedItem> items;
    for (const auto& mt : edit.markerTracks()) {
        if (!mt.visible) continue;
        for (const auto& m : mt.markers) {
            // RB-4 only wires Sample-mode positions; skip others (they'd
            // misplace). Resolution for other modes lands later.
            if (m.posMode != MarkerPosMode::Sample) continue;
            items.push_back({&m, &mt});
        }
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool laneHovered =
        mouse.x >= origin.x + gutterWidth && mouse.x <= origin.x + totalWidth &&
        mouse.y >= origin.y && mouse.y <= origin.y + laneHeight;
    // Option-click deletes a marker, matching the gutter's option-click-to-
    // reset on gain and pan: the modifier means "take this back" throughout.
    const bool altHeld = ImGui::GetIO().KeyAlt;
    // `items` holds raw pointers into the marker vectors and the blocks below
    // walk it again. Removing inside the loop would dangle every one of them,
    // so the deletion is recorded here and executed once the lane has finished
    // drawing.
    std::string pendingDeleteTrackId;
    std::string pendingDeleteMarkerId;

    // Draw markers.
    for (const auto& [m, mt] : items) {
        ImU32 col = parseHexColor(m->color, markerColor(m->kind));
        // Compute the marker's X. If this marker is the one being dragged,
        // apply a live offset from the drag start so it visually follows the
        // mouse before the commit lands on release.
        int64_t renderPos = m->position;
        bool isBeingDragged =
            view.isDragging(TimelineViewState::DragKind::Marker) &&
            view.selectedClipId == m->id;
        if (isBeingDragged) {
            double dxPx = mouse.x - view.markerDragStartX;
            renderPos += static_cast<int64_t>(dxPx * samplesPerPixel);
        }
        double mx = origin.x + gutterWidth +
            (renderPos - scrollSamples) / samplesPerPixel;
        bool isRegion = m->length > 0;
        bool selected = view.selectedClipId == m->id;

        if (isRegion) {
            double mw = static_cast<double>(m->length) / samplesPerPixel;
            if (mx + mw < origin.x + gutterWidth) continue;
            if (mx > origin.x + totalWidth) continue;
            // Region bar — translucent fill (alpha 90) + opaque border.
            // IM_COL32 is (R,G,B,A) packed; we already have col as ImU32, so
            // build the translucent variant by repacking with reduced alpha.
            // col layout (ImGui's IM_COL32): A<<24 | B<<16 | G<<8 | R.
            ImU32 fillCol = (col & 0x00FFFFFF) | (90u << 24);
            dl->AddRectFilled(ImVec2(mx, origin.y + 6),
                              ImVec2(mx + mw, origin.y + laneHeight - 4), fillCol);
            dl->AddRect(ImVec2(mx, origin.y + 6),
                        ImVec2(mx + mw, origin.y + laneHeight - 4), col);
        } else {
            if (mx < origin.x + gutterWidth) continue;
            if (mx > origin.x + totalWidth) continue;
            // Flag triangle pointing down.
            dl->AddTriangleFilled(ImVec2(mx - 5, origin.y + 2),
                                  ImVec2(mx + 5, origin.y + 2),
                                  ImVec2(mx,     origin.y + 10), col);
            dl->AddLine(ImVec2(mx, origin.y + 10),
                        ImVec2(mx, origin.y + laneHeight - 2), col);
        }

        // Label (right of the marker).
        if (!m->name.empty() && mx + 8 < origin.x + totalWidth) {
            dl->AddText(ImVec2(mx + 8, origin.y + 4),
                        C(pal.text), m->name.c_str());
        }

        // Hit-test + drag.
        struct Rect { ImVec2 a, b; bool hit(ImVec2 p) const {
            return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; } };
        Rect hit = isRegion
            ? Rect{ImVec2(mx, origin.y + 4), ImVec2(mx + std::max(8.0, static_cast<double>(m->length) / samplesPerPixel), origin.y + laneHeight - 4)}
            : Rect{ImVec2(mx - 6, origin.y), ImVec2(mx + 6, origin.y + laneHeight)};
        const bool markerHovered = laneHovered && hit.hit(mouse);
        // Option-click is invisible without a cue, so arm it visibly: while
        // the modifier is down, the marker under the pointer wears the danger
        // colour and clicking removes it rather than dragging it.
        if (markerHovered && altHeld) {
            dl->AddRect(hit.a, hit.b, C(pal.danger), 2.0f, 0, 1.5f);
        }
        if (markerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (altHeld) {
                pendingDeleteTrackId = mt->id;
                pendingDeleteMarkerId = m->id;
            } else {
                view.selectedClipId = m->id;     // reuse the selection field
                view.dragClipOriginalStart = m->position;
                view.dragKind = TimelineViewState::DragKind::Marker;
                view.dragOriginalTrackId = mt->id; // reuse for marker track id
                view.markerDragStartX = mouse.x;   // for live drag feedback
            }
        }
    }

    // Handle active marker drag (commit on release). Guarded on the drag KIND,
    // not just "a drag is happening": this lane draws before the track rows, so
    // without the check it cancelled every clip drag on mouse-up.
    if (view.isDragging(TimelineViewState::DragKind::Marker) &&
        !view.selectedClipId.empty()) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Find the selected marker and compute its new position from mouse X.
            for (const auto& mt : edit.markerTracks()) {
                for (const auto& m : mt.markers) {
                    if (m.id == view.selectedClipId) {
                        double localX = mouse.x - (origin.x + gutterWidth);
                        int64_t newPos = static_cast<int64_t>(
                            std::max(0.0, scrollSamples + localX * samplesPerPixel));
                        undo.execute(std::make_unique<editing::MoveMarkerCommand>(
                            mt.id, m.id, newPos, m.length));
                        break;
                    }
                }
            }
            view.dragKind = TimelineViewState::DragKind::None;
            view.selectedClipId.clear();
            view.dragOriginalTrackId.clear();
        }
    }

    // Double-click empty lane to add a point marker on the first marker track.
    if (laneHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // Make sure we didn't hit an existing marker (those capture the click).
        bool hitMarker = false;
        for (const auto& [m, mt] : items) {
            double mx = origin.x + gutterWidth +
                (m->position - scrollSamples) / samplesPerPixel;
            if (std::fabs(mouse.x - mx) < 8.0) { hitMarker = true; break; }
        }
        if (!hitMarker) {
            const auto& target = edit.markerTracks().front();
            double localX = mouse.x - (origin.x + gutterWidth);
            int64_t pos = static_cast<int64_t>(
                std::max(0.0, scrollSamples + localX * samplesPerPixel));
            Marker marker;
            marker.name = "Marker";
            marker.position = pos;
            undo.execute(std::make_unique<editing::AddMarkerCommand>(target.id, marker));
        }
    }

    // Right-click the lane for a context menu: add point marker, add loop
    // region (default 1s), or clear loop.
    if (laneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        double localX = mouse.x - (origin.x + gutterWidth);
        int64_t clickPos = static_cast<int64_t>(
            std::max(0.0, scrollSamples + localX * samplesPerPixel));
        ImGui::OpenPopup("##marker_ctx");
        // Stash the click position for the menu handlers.
        view.dragClipOriginalStart = clickPos; // reuse as temp scratch
    }
    if (ImGui::BeginPopup("##marker_ctx")) {
        int64_t pos = view.dragClipOriginalStart; // the stashed click position
        const auto& target = edit.markerTracks().front();
        if (ImGui::MenuItem("Add point marker")) {
            Marker m; m.name = "Marker"; m.position = pos;
            undo.execute(std::make_unique<editing::AddMarkerCommand>(target.id, m));
        }
        if (ImGui::MenuItem("Add loop region (1s)")) {
            Marker m;
            m.name = "Loop";
            m.kind = MarkerKind::Loop;
            m.position = pos;
            m.length = 48000; // 1s @ 48k
            undo.execute(std::make_unique<editing::AddMarkerCommand>(target.id, m));
        }
        if (edit.activeLoopMarker() && ImGui::MenuItem("Clear loop")) {
            // Remove all loop markers.
            for (const auto& mt : edit.markerTracks()) {
                for (const auto& m : mt.markers) {
                    if (m.kind == MarkerKind::Loop) {
                        undo.execute(std::make_unique<editing::RemoveMarkerCommand>(mt.id, m.id));
                    }
                }
            }
        }
        ImGui::EndPopup();
    }

    // Single-click on a marker seeks the transport to it (navigation). Skipped
    // during a drag so the playhead doesn't jump while you're moving a marker,
    // and while Option is down — that click is a delete, and seeking to a
    // marker the same click removes is a jump to nowhere.
    if (laneHovered && !altHeld && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !view.isDragging()) {
        for (const auto& [m, mt] : items) {
            double mx = origin.x + gutterWidth +
                (m->position - scrollSamples) / samplesPerPixel;
            if (std::fabs(mouse.x - mx) < 8.0) {
                transport.seek(m->position);
                break;
            }
        }
    }

    // Deferred from the draw loop, where `items` still pointed into the vector
    // this invalidates. Undo restores the marker with its original id.
    if (!pendingDeleteMarkerId.empty()) {
        undo.execute(std::make_unique<editing::RemoveMarkerCommand>(
            pendingDeleteTrackId, pendingDeleteMarkerId));
    }

    return laneHeight;
}

// ─── Video lane ─────────────────────────────────────────────────────────────

float drawVideoLane(const document::Edit& edit,
                    engine::Transport& transport,
                    TimelineViewState& view,
                    ImVec2 origin,
                    float totalWidth,
                    float gutterWidth,
                    double scrollSamples,
                    double samplesPerPixel) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float laneHeight = 40.0f;
    const auto& pal = theme::palette();

    // No picture, no lane. The video panel owns onboarding, so an empty strip
    // here would only be a labelled hole in the arrangement.
    if (edit.videoTracks().empty()) {
        return 0.0f;
    }

    // Background — distinct from tracks (slightly darker).
    dl->AddRectFilled(origin, ImVec2(origin.x + totalWidth, origin.y + laneHeight),
                      C(pal.bgElevated));
    dl->AddLine(ImVec2(origin.x, origin.y + laneHeight),
                ImVec2(origin.x + totalWidth, origin.y + laneHeight),
                C(pal.border));
    // Gutter label.
    dl->AddText(ImVec2(origin.x + 10, origin.y + 6), C(pal.textMuted), "Video");

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool laneHovered =
        mouse.x >= origin.x + gutterWidth && mouse.x <= origin.x + totalWidth &&
        mouse.y >= origin.y && mouse.y <= origin.y + laneHeight;

    // Draw all video clips across all visible video tracks (flattened into
    // one lane for RB-6 — per-track stacking comes if multi-track is needed).
    int clipCount = 0;
    for (const auto& vt : edit.videoTracks()) {
        if (!vt.visible) continue;
        for (const auto& clip : vt.clips) {
            double clipX = origin.x + gutterWidth +
                (clip.timelineStart - scrollSamples) / samplesPerPixel;
            int64_t len = (clip.length > 0)
                ? clip.length
                : static_cast<int64_t>(clip.durationSeconds * 48000.0);
            double clipW = static_cast<double>(len) / samplesPerPixel;
            if (clipW < 2) clipW = 2;
            if (clipX + clipW < origin.x + gutterWidth) { ++clipCount; continue; }
            if (clipX > origin.x + totalWidth) { ++clipCount; continue; }

            // The video role remains distinct from audio without bypassing
            // the shared palette that keeps all timeline media in register.
            ImU32 bodyCol = C(pal.clipVideo);
            ImU32 borderCol = C(pal.clipVideoBorder);
            bool isSel = view.selectedClipId == clip.id;
            if (isSel) {
                borderCol = C(pal.accent);
            }
            dl->AddRectFilled(ImVec2(clipX, origin.y + 6),
                              ImVec2(clipX + clipW, origin.y + laneHeight - 4),
                              bodyCol, 2.0f);
            dl->AddRect(ImVec2(clipX, origin.y + 6),
                        ImVec2(clipX + clipW, origin.y + laneHeight - 4),
                        borderCol, 2.0f);
            // Name label (truncated to clip width).
            if (clipW > 30) {
                dl->AddText(ImVec2(clipX + 6, origin.y + 10),
                            C(pal.text), clip.name.c_str());
            }
            // Drag to move: start drag on click.
            if (laneHovered && mouse.x >= clipX && mouse.x <= clipX + clipW &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                view.selectedClipId = clip.id;
                view.dragKind = TimelineViewState::DragKind::VideoClip;
                view.dragOriginalTrackId = vt.id;
                view.dragClipOriginalStart = clip.timelineStart;
                view.markerDragStartX = mouse.x;
            }
            ++clipCount;
        }
    }

    // Handle active video clip drag (live move + commit on release). Same
    // reasoning as the marker lane: only this lane's own drags belong here.
    if (view.isDragging(TimelineViewState::DragKind::VideoClip) &&
        !view.selectedClipId.empty()) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Find the selected video clip and move it.
            double localX = mouse.x - (origin.x + gutterWidth);
            int64_t newPos = static_cast<int64_t>(
                std::max(0.0, scrollSamples + localX * samplesPerPixel));
            for (auto& vt : const_cast<std::vector<document::VideoTrack>&>(edit.videoTracks())) {
                for (auto& c : vt.clips) {
                    if (c.id == view.selectedClipId) {
                        c.timelineStart = std::max<int64_t>(0, newPos);
                        break;
                    }
                }
            }
            view.dragKind = TimelineViewState::DragKind::None;
            view.selectedClipId.clear();
        }
    }

    // Click empty lane area to seek (only if not dragging a clip).
    if (laneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !view.isDragging()) {
        bool hitClip = false;
        for (const auto& vt : edit.videoTracks()) {
            for (const auto& clip : vt.clips) {
                double cx = origin.x + gutterWidth +
                    (clip.timelineStart - scrollSamples) / samplesPerPixel;
                int64_t len = (clip.length > 0)
                    ? clip.length
                    : static_cast<int64_t>(clip.durationSeconds * 48000.0);
                double cw = static_cast<double>(len) / samplesPerPixel;
                if (mouse.x >= cx && mouse.x <= cx + cw) {
                    hitClip = true;
                    break;
                }
            }
            if (hitClip) break;
        }
        if (!hitClip) {
            double localX = mouse.x - (origin.x + gutterWidth);
            int64_t seekTo = static_cast<int64_t>(
                std::max(0.0, scrollSamples + localX * samplesPerPixel));
            transport.seek(seekTo);
        }
    }

    return laneHeight;
}

} // namespace dave::gui
