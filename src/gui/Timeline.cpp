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

// Helpers converting Palette ImVec4 -> ImU32 (ImDrawList wants packed colors).
static inline ImU32 C(const ImVec4& v) {
    return IM_COL32(
        int(v.x * 255), int(v.y * 255), int(v.z * 255), int(v.w * 255));
}

namespace {
constexpr size_t kPeakCacheBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxBucketsPerLevel = 1u << 20;
}

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
    const float tracksRegionHeight =
        static_cast<float>(tracks.size()) * trackHeight;
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
    const double sr = 48000.0;
    // Keep labelled ticks roughly a label-width apart. This is chosen from
    // zoom rather than a fixed second grid, so both sample-level edits and
    // long-form timelines retain an intelligible ruler.
    constexpr double majorSeconds[] = {
        0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0,
        10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0
    };
    constexpr double targetMajorPixels = 120.0;
    double majorSecondsStep = majorSeconds[std::size(majorSeconds) - 1];
    for (double candidate : majorSeconds) {
        if (candidate * sr / view.samplesPerPixel >= targetMajorPixels) {
            majorSecondsStep = candidate;
            break;
        }
    }
    const int64_t majorStep = std::max<int64_t>(1,
        static_cast<int64_t>(std::llround(majorSecondsStep * sr)));
    const int64_t minorStep = std::max<int64_t>(1, majorStep / 5);
    const int64_t firstMinor = std::max<int64_t>(0,
        static_cast<int64_t>(std::floor(view.scrollSamples / minorStep)));
    ImFont* rulerFont = theme::fonts().monoSmall != nullptr
        ? theme::fonts().monoSmall : ImGui::GetFont();
    const float rulerFontSize = static_cast<float>(theme::typeScale().caption);
    dl->PushClipRect(ImVec2(origin.x + gutterWidth, origin.y),
                     ImVec2(origin.x + totalWidth, origin.y + timelineHeight), true);
    for (int64_t tick = firstMinor; ; ++tick) {
        const int64_t sample = tick * minorStep;
        double x = origin.x + gutterWidth +
            (sample - view.scrollSamples) / view.samplesPerPixel;
        if (x > origin.x + totalWidth) break;
        if (x < origin.x + gutterWidth) continue;
        const bool major = sample % majorStep == 0;
        const float tickTop = major ? origin.y + 15.0f : origin.y + 22.0f;
        dl->AddLine(ImVec2(x, tickTop), ImVec2(x, origin.y + timelineHeight),
                    C(major ? pal.borderStrong : pal.border));
        if (major) {
            const std::string label = formatTimecode(sample, view.tcMode);
            dl->AddText(rulerFont, rulerFontSize, ImVec2(x + 4.0f, origin.y + 2.0f),
                        C(pal.textMuted), label.c_str());
        }
    }
    dl->PopClipRect();

    // Marker lane was already drawn above (before the InvisibleButton). Reuse
    // its height to compute the track-row region.
    const float tracksTop = origin.y + timelineHeight + markerLaneHeight;
    const bool gutterHovered =
        windowHovered &&
        mouse.x >= origin.x && mouse.x <= origin.x + gutterWidth &&
        mouse.y >= tracksTop &&
        mouse.y <= tracksTop + tracksRegionHeight;

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

        // Double-click the name to rename (inline text input).
        {
            // Unique ID for the rename input so each track has its own state.
            char renameId[32];
            std::snprintf(renameId, sizeof(renameId), "##rename_%zu", ti);
            // Check if this track is being renamed.
            bool& renaming = const_cast<bool&>(view.isRenaming); // reuse single flag
            int& renameIdx = const_cast<int&>(view.renameTrackIndex);
            bool thisRenaming = renaming && renameIdx == static_cast<int>(ti);

            if (thisRenaming) {
                // Show an InputText in place of the name.
                ImGui::SetCursorScreenPos(
                    ImVec2(origin.x + 20.0f, y + rowPadding));
                ImGui::PushItemWidth(gutterWidth - 88.0f);
                ImGui::SetKeyboardFocusHere();
                char nameBuf[128];
                std::strncpy(nameBuf, track.name.c_str(), sizeof(nameBuf) - 1);
                nameBuf[sizeof(nameBuf) - 1] = '\0';
                ImGui::PushID(static_cast<int>(ti) + 1000);
                if (ImGui::InputText(renameId, nameBuf, sizeof(nameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll)) {
                    const_cast<document::Track&>(track).name = nameBuf;
                    const_cast<document::Edit&>(edit).notifyChanged();
                    renaming = false;
                }
                // Lose focus (click away / Escape) = cancel rename.
                if (!ImGui::IsItemActive() &&
                    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                     ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                    renaming = false;
                }
                ImGui::PopID();
                ImGui::PopItemWidth();
            } else {
                ImFont* nameFont = theme::fonts().label != nullptr
                    ? theme::fonts().label : ImGui::GetFont();
                dl->PushClipRect(
                    ImVec2(origin.x + 20.0f, y),
                    ImVec2(origin.x + gutterWidth - 62.0f,
                           y + headerHeight + rowPadding * 2.0f),
                    true);
                dl->AddText(
                    nameFont, labelHeight,
                    ImVec2(origin.x + 20.0f, y + rowPadding),
                    C(pal.text), track.name.c_str());
                dl->PopClipRect();
            }
            // Double-click on the name area starts rename mode.
            if (gutterHovered &&
                mouse.x >= origin.x + 16.0f &&
                mouse.x <= origin.x + gutterWidth - 62.0f &&
                mouse.y >= y && mouse.y <= y + headerHeight + rowPadding * 2.0f &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renaming = true;
                renameIdx = static_cast<int>(ti);
                view.selectedTrackIndex = static_cast<int>(ti);
            }
        }

        // Mute/solo indicators, drawn to PTXExtractor's spec: 21x17, 3px
        // radius, hairline border, amber for mute and yellow for solo.
        //
        // These are inert. The document model has no mute or solo state yet,
        // so there is nothing to toggle — they are drawn in their inactive
        // state and deliberately rendered dimmed rather than as live controls,
        // because a track header with convincing-looking M/S buttons that
        // silently do nothing is worse than one that shows they aren't wired.
        {
            const ImVec2 msSize(21.0f, 17.0f);
            const float msGap = 3.0f;
            float msX = origin.x + gutterWidth - (msSize.x * 2.0f + msGap) - 12.0f;
            const float msY = y + rowPadding;
            const ImU32 inactiveFill = C(pal.trackControlInactive);
            const ImU32 borderCol = C(pal.border);
            const ImU32 labelCol = IM_COL32(
                static_cast<int>(pal.textSubtle.x * 255),
                static_cast<int>(pal.textSubtle.y * 255),
                static_cast<int>(pal.textSubtle.z * 255), 122);
            const char* labels[2] = {"M", "S"};
            for (int b = 0; b < 2; ++b) {
                const ImVec2 bMin(msX, msY);
                const ImVec2 bMax(msX + msSize.x, msY + msSize.y);
                dl->AddRectFilled(bMin, bMax, inactiveFill, 3.0f);
                dl->AddRect(bMin, bMax, borderCol, 3.0f);
                const ImVec2 ts = ImGui::CalcTextSize(labels[b]);
                dl->AddText(ImVec2(msX + (msSize.x - ts.x) * 0.5f,
                                   msY + (msSize.y - ts.y) * 0.5f),
                            labelCol, labels[b]);
                msX += msSize.x + msGap;
            }
        }

        // Click the gutter to select this track.
        if (gutterHovered &&
            mouse.y >= y && mouse.y <= y + trackHeight &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.selectedTrackIndex = static_cast<int>(ti);
        }

        // Interactive gain + pan controls in the gutter.
        // Option-click (Alt-click) resets to default: gain=0 dB, pan=center.
        ImGui::PushID(static_cast<int>(ti));
        float controlY = y + rowPadding + headerHeight + rowGap;
        constexpr float controlX = 12.0f;
        constexpr float controlLabelW = 64.0f;
        const float sliderW =
            gutterWidth - controlX - 10.0f - controlLabelW - 6.0f;
        {
            // Gain: dB slider from -60 to +6 dB. Convert to/from linear.
            float gainDb = 20.0f * std::log10(std::max(0.0001f, static_cast<float>(track.gain)));
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
                float linear = std::pow(10.0f, gainDb / 20.0f);
                const_cast<document::Track&>(track).gain = linear;
                const_cast<document::Edit&>(edit).notifyChanged();
            }
            if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) {
                gainDb = 0.0f;
                const_cast<document::Track&>(track).gain = 1.0f;
                const_cast<document::Edit&>(edit).notifyChanged();
            }
            ImGui::PopItemWidth();
        }
        controlY += compactControlHeight + rowGap;
        // Pan: -1=L to +1=R. Option-click resets to center (0.0).
        float panVal = static_cast<float>(track.pan);
        char panText[16];
        std::snprintf(panText, sizeof(panText), "%.1f", panVal);
        dl->AddText(ImVec2(origin.x + controlX, controlY + 1.0f),
                    C(pal.textMuted), "Pan");
        const float panTextWidth = ImGui::CalcTextSize(panText).x;
        dl->AddText(ImVec2(origin.x + controlX + controlLabelW - panTextWidth - 4.0f,
                           controlY + 1.0f), C(pal.textSubtle), panText);
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + controlX + controlLabelW, controlY));
        ImGui::PushItemWidth(sliderW);
        if (ImGui::SliderFloat("##pan", &panVal, -1.0f, 1.0f, "")) {
            const_cast<document::Track&>(track).pan = panVal;
            const_cast<document::Edit&>(edit).notifyChanged();
        }
        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) {
            panVal = 0.0f;
            const_cast<document::Track&>(track).pan = 0.0f;
            const_cast<document::Edit&>(edit).notifyChanged();
        }
        ImGui::PopItemWidth();
        ImGui::PopID();

        for (const auto& clip : track.clips) {
            double clipX = origin.x + gutterWidth +
                (clip.timelineStart - view.scrollSamples) / view.samplesPerPixel;
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
                view.dragOriginalTrackId = track.id;
                view.dragging = true;
            }
            // Right-click: context menu (split, delete).
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                ImGui::OpenPopup("##clip_ctx");
            }
        }
    }
    ImGui::PopStyleVar();

    const float addTrackY =
        tracksTop + static_cast<float>(tracks.size()) * trackHeight;
    constexpr float addTrackHeight = 42.0f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, addTrackY));
    ImGui::InvisibleButton(
        "##addTrackTimeline", ImVec2(totalWidth, addTrackHeight));
    const bool addTrackHovered =
        ImGui::IsItemHovered() || ImGui::IsItemActive();
    const bool addTrackRequested =
        ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (addTrackHovered) {
        dl->AddRectFilled(
            ImVec2(origin.x, addTrackY),
            ImVec2(origin.x + totalWidth, addTrackY + addTrackHeight),
            C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.08f)));
    }
    const ImU32 ghostBorder = C(
        addTrackHovered ? pal.accent : pal.borderStrong);
    constexpr float dash = 7.0f;
    constexpr float dashGap = 5.0f;
    for (float x = origin.x + 8.0f;
         x < origin.x + totalWidth - 8.0f; x += dash + dashGap) {
        dl->AddLine(
            ImVec2(x, addTrackY + 5.0f),
            ImVec2(std::min(x + dash, origin.x + totalWidth - 8.0f),
                   addTrackY + 5.0f),
            ghostBorder);
        dl->AddLine(
            ImVec2(x, addTrackY + addTrackHeight - 5.0f),
            ImVec2(std::min(x + dash, origin.x + totalWidth - 8.0f),
                   addTrackY + addTrackHeight - 5.0f),
            ghostBorder);
    }
    const char* addTrackLabel = "+ Add track";
    ImFont* addTrackFont = theme::fonts().label != nullptr
        ? theme::fonts().label : ImGui::GetFont();
    const ImVec2 addTrackLabelSize = addTrackFont->CalcTextSizeA(
        labelHeight, FLT_MAX, 0.0f, addTrackLabel);
    dl->AddText(
        addTrackFont, labelHeight,
        ImVec2(origin.x + (totalWidth - addTrackLabelSize.x) * 0.5f,
               addTrackY + (addTrackHeight - addTrackLabelSize.y) * 0.5f),
        C(addTrackHovered ? pal.accentStrong : pal.textMuted),
        addTrackLabel);
    const float videoLaneY = addTrackY + addTrackHeight;
    // The spare canvas is deliberately the fastest way to create another
    // track: the GLFW drop callback imports each WAV into a fresh lane.
    constexpr float videoLaneHeight = 40.0f;
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
        windowHovered &&
        mouse.x >= origin.x + gutterWidth &&
        mouse.x <= origin.x + totalWidth &&
        mouse.y >= tracksTop &&
        mouse.y <= origin.y + totalHeight &&
        !(mouse.y >= addTrackY &&
          mouse.y <= addTrackY + addTrackHeight) &&
        !(mouse.y >= videoLaneY &&
          mouse.y <= videoLaneY + 40.0f);

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
    static int64_t dragStartMouseX = 0;
    static int64_t dragStartMouseY = 0;
    if (view.dragging && view.selectedClipId.empty() == false) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragStartMouseX = static_cast<int64_t>(mouse.x);
            dragStartMouseY = static_cast<int64_t>(mouse.y);
        }
        // Figure out which track the mouse is currently over (by Y).
        int targetTrackIndex = -1;
        if (mouse.y >= tracksTop) {
            targetTrackIndex = static_cast<int>((mouse.y - tracksTop) / trackHeight);
            if (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int>(tracks.size())) {
                targetTrackIndex = -1;
            }
        }

        // Live move on the original clip (horizontal only during drag; the
        // actual track move happens on commit so undo is clean).
        int64_t dxPx = static_cast<int64_t>(mouse.x) - dragStartMouseX;
        int64_t dxSamples = static_cast<int64_t>(dxPx * view.samplesPerPixel);
        for (auto& track : const_cast<std::vector<document::Track>&>(tracks)) {
            for (auto& clip : track.clips) {
                if (clip.id == view.selectedClipId) {
                    int64_t raw = std::max<int64_t>(0, view.dragClipOriginalStart + dxSamples);
                    clip.timelineStart = snapPosition(raw);
                    break;
                }
            }
        }
        // Visual hint: highlight the track being hovered during drag.
        if (targetTrackIndex >= 0) {
            float ty = tracksTop + static_cast<float>(targetTrackIndex) * trackHeight;
            dl->AddRectFilled(ImVec2(origin.x + gutterWidth, ty),
                              ImVec2(origin.x + totalWidth, ty + trackHeight),
                              C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.12f)));
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Commit via command. If the mouse ended on a different track, move it.
            std::string targetTrackId = view.dragOriginalTrackId;
            if (targetTrackIndex >= 0) {
                targetTrackId = tracks[targetTrackIndex].id;
            }
            for (auto& track : const_cast<std::vector<document::Track>&>(tracks)) {
                for (auto& clip : track.clips) {
                    if (clip.id == view.selectedClipId) {
                        std::string newTrackArg =
                            (targetTrackId != view.dragOriginalTrackId) ? targetTrackId : "";
                        undo.execute(std::make_unique<editing::MoveClipCommand>(
                            view.dragOriginalTrackId, clip.id, clip.timelineStart, newTrackArg));
                        break;
                    }
                }
            }
            view.dragging = false;
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

    // Ruler: click seeks; drag scrubs (continuous seek while held).
    if (rulerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        seekToMouseX();
    }
    if (rulerHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        seekToMouseX();
    }

    // Track area: click empty space seeks (but not on clips — those drag).
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.dragging) {
        bool hitClip = false;
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                double clipX = origin.x + gutterWidth +
                    (clip.timelineStart - view.scrollSamples) / view.samplesPerPixel;
                double clipW = static_cast<double>(clip.length) / view.samplesPerPixel;
                if (mouse.x >= clipX && mouse.x <= clipX + clipW) { hitClip = true; break; }
            }
            if (hitClip) break;
        }
        if (!hitClip) {
            // Start a selection drag (instead of just seeking).
            double localX = mouse.x - (origin.x + gutterWidth);
            int64_t s = static_cast<int64_t>(
                view.scrollSamples + std::max(0.0, localX) * view.samplesPerPixel);
            view.selectionStart = s;
            view.selectionEnd = s;
            view.isSelecting = true;
            view.hasSelection = true;
        }
    }

    // Update selection end while dragging.
    if (view.isSelecting && canvasHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        double localX = mouse.x - (origin.x + gutterWidth);
        view.selectionEnd = static_cast<int64_t>(
            view.scrollSamples + std::max(0.0, localX) * view.samplesPerPixel);
    }
    // End selection on mouse release.
    if (view.isSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        view.isSelecting = false;
        // If the selection is tiny (just a click, not a drag), treat as seek + clear selection.
        if (std::abs(view.selectionEnd - view.selectionStart) <
            static_cast<int64_t>(4 * view.samplesPerPixel)) {
            transport.seek(view.selectionStart);
            view.hasSelection = false;
        }
    }
    // Clear selection on Escape or clicking elsewhere.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        view.hasSelection = false;
    }

    // Draw selection region (translucent highlight).
    if (view.hasSelection) {
        int64_t selMin = std::min(view.selectionStart, view.selectionEnd);
        int64_t selMax = std::max(view.selectionStart, view.selectionEnd);
        double selX1 = origin.x + gutterWidth + (selMin - view.scrollSamples) / view.samplesPerPixel;
        double selX2 = origin.x + gutterWidth + (selMax - view.scrollSamples) / view.samplesPerPixel;
        if (selX2 > selX1) {
            dl->AddRectFilled(
                ImVec2(selX1, origin.y + timelineHeight),
                ImVec2(selX2, origin.y + totalHeight),
                C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.15f)));
            dl->AddRect(
                ImVec2(selX1, origin.y + timelineHeight),
                ImVec2(selX2, origin.y + totalHeight),
                C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.5f)));
            // Time readout at the top of the selection.
            double durSec = (selMax - selMin) / 48000.0;
            char selLabel[32];
            std::snprintf(selLabel, sizeof(selLabel), "%.2fs", durSec);
            dl->AddText(ImVec2((selX1 + selX2) / 2 - 15, origin.y + timelineHeight + 2),
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
                    C(pal.accentDeep), 5.0f);
        dl->AddLine(ImVec2(playheadX, origin.y + timelineHeight),
                    ImVec2(playheadX, origin.y + totalHeight),
                    C(pal.accentStrong), 2.0f);
        dl->AddRectFilled(
            ImVec2(playheadX - 5.0f, origin.y + timelineHeight - 8.0f),
            ImVec2(playheadX + 5.0f, origin.y + timelineHeight - 3.0f),
            C(pal.accentStrong), 2.0f);
        dl->AddTriangleFilled(
            ImVec2(playheadX - 8, origin.y + timelineHeight - 3),
            ImVec2(playheadX + 8, origin.y + timelineHeight - 3),
            ImVec2(playheadX, origin.y + timelineHeight + 4), C(pal.accentStrong));
    }

    // Video lane — a strip at the bottom of the timeline showing video clips.
    // Placed below the track rows, above the scroll/zoom handler.
    {
        const float videoLaneHeight = drawVideoLane(
            edit, transport, view, ImVec2(origin.x, videoLaneY),
            totalWidth, gutterWidth, view.scrollSamples,
            view.samplesPerPixel);
        // Register the absolute-drawn lane with ImGui's content extent so a
        // long track list remains vertically scrollable down to its add row.
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x, videoLaneY + videoLaneHeight));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }

    // Scroll / zoom.
    double wheel = ImGui::GetIO().MouseWheel;
    if (canvasHovered && wheel != 0.0) {
        if (ImGui::GetIO().KeyCtrl) {
            view.samplesPerPixel = std::clamp(view.samplesPerPixel - wheel * 10.0, 4.0, 50000.0);
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

    // Draw markers.
    for (const auto& [m, mt] : items) {
        ImU32 col = parseHexColor(m->color, markerColor(m->kind));
        // Compute the marker's X. If this marker is the one being dragged,
        // apply a live offset from the drag start so it visually follows the
        // mouse before the commit lands on release.
        int64_t renderPos = m->position;
        bool isBeingDragged = view.dragging && view.selectedClipId == m->id;
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
        if (laneHovered && hit.hit(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.selectedClipId = m->id;     // reuse the selection field
            view.dragClipOriginalStart = m->position;
            view.dragging = true;
            view.dragOriginalTrackId = mt->id; // reuse for marker track id
            view.markerDragStartX = mouse.x;   // for live drag feedback
        }
    }

    // Handle active marker drag (commit on release).
    if (view.dragging && !view.selectedClipId.empty()) {
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
            view.dragging = false;
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
    // during a drag so the playhead doesn't jump while you're moving a marker.
    if (laneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.dragging) {
        for (const auto& [m, mt] : items) {
            double mx = origin.x + gutterWidth +
                (m->position - scrollSamples) / samplesPerPixel;
            if (std::fabs(mouse.x - mx) < 8.0) {
                transport.seek(m->position);
                break;
            }
        }
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

    // Background — distinct from tracks (slightly darker).
    dl->AddRectFilled(origin, ImVec2(origin.x + totalWidth, origin.y + laneHeight),
                      C(pal.bgElevated));
    dl->AddLine(ImVec2(origin.x, origin.y + laneHeight),
                ImVec2(origin.x + totalWidth, origin.y + laneHeight),
                C(pal.border));
    // Gutter label.
    dl->AddText(ImVec2(origin.x + 10, origin.y + 6), C(pal.textMuted), "Video");

    // The picture panel owns onboarding. Repeating it here splits the empty
    // state into two unrelated fragments and makes this lane read as an error.
    if (edit.videoTracks().empty()) {
        return laneHeight;
    }

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
                view.dragging = true;
                view.dragOriginalTrackId = vt.id;
                view.dragClipOriginalStart = clip.timelineStart;
                view.markerDragStartX = mouse.x;
            }
            ++clipCount;
        }
    }

    // Handle active video clip drag (live move + commit on release).
    if (view.dragging && !view.selectedClipId.empty()) {
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
            view.dragging = false;
            view.selectedClipId.clear();
        }
    }

    // Click empty lane area to seek (only if not dragging a clip).
    if (laneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.dragging) {
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
