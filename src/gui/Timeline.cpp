// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/Timeline.h"

#include "document/MusicalTime.h"
#include "editing/Commands.h"
#include "gui/RoutingViewModel.h"
#include "gui/SideRail.h"
#include "gui/Theme.h"
#include "gui/TrackColorPicker.h"

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
std::vector<document::VolumeAutomationPoint> automationErase(
    const std::vector<document::VolumeAutomationPoint>& points,
    int64_t loSample, int64_t hiSample) {
    std::vector<document::VolumeAutomationPoint> result;
    result.reserve(points.size());
    for (const auto& point : points) {
        if (point.sample >= loSample && point.sample <= hiSample) continue;
        result.push_back(point);
    }
    return result;
}

double automationCurveShape(double t, double steepness, bool flipped) {
    t = std::clamp(t, 0.0, 1.0);
    const double exponent = std::clamp(steepness, 0.1, 12.0);
    return flipped ? 1.0 - std::pow(1.0 - t, exponent)
                   : std::pow(t, exponent);
}

double automationSteepnessForDrag(double latched, float dx) {
    // 55 px doubles it, so the useful range is a short flick rather than a
    // reach across the lane. Exponential rather than additive so the gesture
    // feels the same at 0.3 as at 6 — additive would jam against the floor.
    constexpr float kPixelsPerDoubling = 55.0f;
    const double scaled = std::clamp(latched, 0.1, 12.0) *
                          std::pow(2.0, dx / kPixelsPerDoubling);
    return std::clamp(scaled, 0.1, 12.0);
}

std::string formatTimecode(int64_t samples, TimecodeMode mode,
                           double sr, double fps, double bpm,
                           const std::vector<document::TimeSignature>* meter,
                           const std::vector<document::TempoChange>* tempo) {
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
            // Through the meter map, so a 3/4 bar reads as three beats and
            // every bar line after a change lands where the map puts it.
            static const std::vector<document::TimeSignature> kCommonTime{
                document::TimeSignature{1, 4, 4}};
            const auto& map = meter != nullptr ? *meter : kCommonTime;
            const auto position = tempo != nullptr
                ? document::barsBeatsAtSample(samples, sr, *tempo, map)
                : document::barsBeatsAtSample(samples, sr, bpm, map);
            // Pipes, not dots: a dot reads as a decimal point, and "2.3.480"
            // looks like one number where "2|3|480" reads as three fields.
            std::snprintf(buf, sizeof(buf), "%d|%d|%03d", position.bar,
                          position.beat, position.tick);
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

std::string formatAutomationDrawValue(AutomationParameter parameter,
                                      double value) {
    if (parameter == AutomationParameter::Pan) {
        return theme::formatPan(document::clampPanAutomation(value));
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%+.1f dB",
                  document::clampVolumeAutomationDb(value));
    return text;
}

namespace {

// How wide a clip's trim handles are, and which gesture a press inside a clip
// starts. Capped at a third of the clip so a very short one stays draggable
// rather than being nothing but handle.
constexpr float kTrimHandlePx = 6.0f;

TimelineViewState::DragKind clipGestureAt(float mouseX, float clipLeft,
                                          float clipRight) {
    const float handle = std::min(kTrimHandlePx, (clipRight - clipLeft) / 3.0f);
    if (mouseX <= clipLeft + handle) {
        return TimelineViewState::DragKind::TrimStart;
    }
    if (mouseX >= clipRight - handle) {
        return TimelineViewState::DragKind::TrimEnd;
    }
    // None means "not an edge" — the caller substitutes its own move kind.
    return TimelineViewState::DragKind::None;
}


// The divisions a format counts in, ascending, in samples. One definition
// serves both the grid lines and the snap increment, so a selection edge can
// never land somewhere the grid says is not a division.
std::vector<double> formatLadder(TimecodeMode mode, double sr, double fps,
                                 double bpm, int& subdivisions,
                                 const std::vector<document::TimeSignature>*
                                     meter,
                                 const std::vector<document::TempoChange>*
                                     tempo) {
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
            // The beat at bar 1. A meter change further down the timeline
            // changes bar LENGTHS, which the ruler walks per bar; the ladder
            // only needs a unit to scale by.
            static const std::vector<document::TimeSignature> kCommonTime{
                document::TimeSignature{1, 4, 4}};
            const auto& map = meter != nullptr ? *meter : kCommonTime;
            const double beat = tempo != nullptr
                ? document::samplesPerBeatAtBar(1, sr, *tempo, map)
                : document::samplesPerBeatAtBar(1, sr, bpm, map);
            for (double b : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
                             128.0, 256.0, 512.0, 1024.0}) {
                ladder.push_back(b * beat);
            }
            subdivisions = std::max(
                1, document::signatureAtBar(map, 1).numerator);
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

// The colour band down the left of every track header. Wide enough to hold a
// disclosure arrow, so the band is both the track's identity colour and the
// control that opens it — one target instead of a stripe plus a separate
// twisty squeezed in beside the name.
// The colour band takes the outer edge, then the meter, then the controls.
// The band is the track's identity, so it reads as the row's left border; the
// meter sits inboard of it, still in its own column so a glance down the
// gutter still catches every track's level at once.
constexpr float kBandX = 8.0f;
constexpr float kBandW = 18.0f;
constexpr float kMeterX = kBandX + kBandW + 5.0f;           // 31
constexpr float kMeterW = 17.0f;                            // two 7 px bars
// Everything else in the gutter starts clear of the meter.
constexpr float kGutterContentX = kMeterX + kMeterW + 6.0f; // 54

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
    const double oldSamplesPerPixel = std::clamp(
        view.samplesPerPixel, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    view.samplesPerPixel = std::clamp(
        newSamplesPerPixel, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    // Before the first frame the widget has not reported its width yet, so
    // there is no stable screen position to preserve. Change the zoom and
    // leave the scroll where it is rather than guessing at an anchor.
    if (view.laneWidthPixels <= 0.0f) return;

    // Keep a visible playhead at exactly the same screen X. Recentring every
    // zoom step makes the edit appear to slide underneath the user. If the
    // playhead is currently off-screen, centring it is the useful recovery
    // behaviour and preserves the previous keyboard-zoom affordance.
    double anchorPixel =
        (static_cast<double>(anchorSample) - view.scrollSamples) /
        oldSamplesPerPixel;
    if (anchorPixel < 0.0 || anchorPixel > view.laneWidthPixels) {
        anchorPixel = view.laneWidthPixels * 0.5;
    }
    view.scrollSamples =
        std::max(0.0, static_cast<double>(anchorSample) -
                          anchorPixel * view.samplesPerPixel);
}

GridStep gridStepFor(TimecodeMode mode, double samplesPerPixel,
                     double sr, double fps, double bpm,
                     const std::vector<document::TimeSignature>* meter,
                     const std::vector<document::TempoChange>* tempo) {
    // Keep labelled divisions roughly a label-width apart whatever the format,
    // so the choice follows zoom rather than a fixed step that goes unreadable
    // at one end of the range and useless at the other.
    constexpr double kTargetMajorPixels = 120.0;
    const double frame = (fps > 0.0) ? sr / fps : sr;
    int subdivisions = 5;
    const std::vector<double> ladder =
        formatLadder(mode, sr, fps, bpm, subdivisions, meter, tempo);
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
                    double fps, double bpm,
                    const std::vector<document::TimeSignature>* meter,
                    const std::vector<document::TempoChange>* tempo) {
    // Three pixels is about the finest a drag can be aimed at. Below that the
    // snap stops being a help and becomes a floor the pointer cannot reach
    // between; above it, the same ladder steps up in the format's own units,
    // so zooming out coarsens the snap to 2 frames, 5 frames, a second —
    // never to an arbitrary sample count.
    constexpr double kTargetSnapPixels = 3.0;
    int subdivisions = 5;
    const std::vector<double> ladder =
        formatLadder(mode, sr, fps, bpm, subdivisions, meter, tempo);
    return std::max<int64_t>(1, std::llround(ladderStepFor(
        ladder, kTargetSnapPixels, samplesPerPixel, 1.0)));
}

int64_t snapSampleToFormat(int64_t sample, TimecodeMode mode,
                           double samplesPerPixel, double sr,
                           double fps, double bpm,
                           const std::vector<document::TimeSignature>* meter,
                           const std::vector<document::TempoChange>* tempo) {
    sample = std::max<int64_t>(0, sample);
    const int64_t step =
        snapStepFor(mode, samplesPerPixel, sr, fps, bpm, meter, tempo);
    if (step <= 1) return sample;

    const int64_t wholeSteps = sample / step;
    const int64_t remainder = sample % step;
    if (remainder >= (step + 1) / 2 &&
        wholeSteps < std::numeric_limits<int64_t>::max() / step) {
        return (wholeSteps + 1) * step;
    }
    return wholeSteps * step;
}

// Helpers converting Palette ImVec4 -> ImU32 (ImDrawList wants packed colors).
static inline ImU32 C(const ImVec4& v) {
    return IM_COL32(
        int(v.x * 255), int(v.y * 255), int(v.z * 255), int(v.w * 255));
}

namespace {
constexpr size_t kPeakCacheBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxBucketsPerLevel = 1u << 20;

// The toolbar and lane cursor share one pencil silhouette so the selected
// tool and the thing under the user's hand cannot drift into different icons.
// `tip` is also the cursor hotspot: drawing begins at the graphite point.
void drawPencilGlyph(ImDrawList* dl, ImVec2 tip, float scale = 1.0f) {
    const auto point = [&](float x, float y) {
        return ImVec2(tip.x + x * scale, tip.y + y * scale);
    };
    const ImU32 outline = IM_COL32(36, 33, 30, 255);
    const ImU32 yellow = IM_COL32(244, 190, 54, 255);
    const ImU32 highlight = IM_COL32(255, 226, 116, 255);
    const ImU32 eraserColor = IM_COL32(224, 126, 143, 255);
    const ImU32 ferruleColor = IM_COL32(172, 181, 181, 255);
    const ImU32 wood = IM_COL32(226, 187, 132, 255);
    const ImU32 graphite = IM_COL32(32, 31, 29, 255);

    const ImVec2 bodyUpper = point(2.2f, -6.4f);
    const ImVec2 bodyLower = point(6.6f, -2.0f);
    const ImVec2 ferruleUpper = point(9.4f, -13.6f);
    const ImVec2 ferruleLower = point(13.8f, -9.2f);
    const ImVec2 eraserUpper = point(12.9f, -17.1f);
    const ImVec2 eraserLower = point(17.3f, -12.7f);

    const ImVec2 body[4] = {
        bodyUpper, ferruleUpper, ferruleLower, bodyLower,
    };
    dl->AddConvexPolyFilled(body, 4, yellow);
    dl->AddTriangleFilled(tip, bodyUpper, bodyLower, wood);

    const ImVec2 ferrule[4] = {
        ferruleUpper, point(11.5f, -15.7f), point(15.9f, -11.3f),
        ferruleLower,
    };
    dl->AddConvexPolyFilled(ferrule, 4, ferruleColor);

    const ImVec2 eraser[4] = {
        eraserUpper, point(15.0f, -19.2f), point(19.4f, -14.8f),
        eraserLower,
    };
    dl->AddConvexPolyFilled(eraser, 4, eraserColor);

    dl->AddTriangleFilled(tip, point(0.7f, -2.1f), point(2.1f, -0.7f),
                          graphite);
    dl->AddLine(point(4.8f, -6.2f), point(10.8f, -12.2f), highlight,
                1.2f * scale);

    const ImVec2 silhouette[7] = {
        tip, bodyUpper, ferruleUpper, eraserUpper, point(19.4f, -14.8f),
        eraserLower, bodyLower,
    };
    dl->AddPolyline(silhouette, 7, outline, ImDrawFlags_Closed,
                    1.4f * scale);
}

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
    const std::string* trackId = nullptr;
    std::string* name = nullptr;
    double* gain = nullptr;
    double* pan = nullptr;
    bool* mute = nullptr;
    bool* solo = nullptr;
    const bool* soloSafe = nullptr;
    // Audio tracks expose record arm. MIDI passes null and retains the exact
    // two-button M/S layout it had before recording existed.
    bool* recordArm = nullptr;
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
            // Match the readout: same font and size, and back the input frame
            // off by one FramePadding so the glyphs land exactly where the name
            // is drawn when not editing — switching to edit shouldn't nudge it,
            // the same fix as the time counter.
            ImFont* nameFont = theme::fonts().label != nullptr
                ? theme::fonts().label : ImGui::GetFont();
            ImGui::PushFont(nameFont, g.labelHeight);
            const float framePadX = ImGui::GetStyle().FramePadding.x;
            const float framePadY = ImGui::GetStyle().FramePadding.y;
            ImGui::SetCursorScreenPos(
                ImVec2(origin.x + kGutterContentX - framePadX,
                       y + g.rowPadding - framePadY));
            ImGui::PushItemWidth(
                g.gutterWidth - (f.recordArm != nullptr ? 118.0f : 88.0f));
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
            ImGui::PopFont();
        } else {
            ImFont* nameFont = theme::fonts().label != nullptr
                ? theme::fonts().label : ImGui::GetFont();
            dl->PushClipRect(
                ImVec2(origin.x + kGutterContentX, y),
                ImVec2(origin.x + g.gutterWidth -
                           (f.recordArm != nullptr ? 110.0f : 86.0f),
                       y + g.headerHeight + g.rowPadding * 2.0f),
                true);
            dl->AddText(nameFont, g.labelHeight,
                        ImVec2(origin.x + kGutterContentX, y + g.rowPadding),
                        C(pal.text), f.name->c_str());
            dl->PopClipRect();
        }
        const bool overName = g.gutterHovered &&
            g.mouse.x >= origin.x + 16.0f &&
            g.mouse.x <= origin.x + g.gutterWidth -
                             (f.recordArm != nullptr ? 110.0f : 86.0f) &&
            g.mouse.y >= y &&
            g.mouse.y <= y + g.headerHeight + g.rowPadding * 2.0f;

        // Right-click the name for the track menu. The colour band next to it
        // already owns right-click for colours, so this is the one other
        // place on the row with nothing bound to it.
        char menuId[32];
        std::snprintf(menuId, sizeof(menuId), "##trackMenu_%d", uid);
        if (overName && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup(menuId);
        }
        if (ImGui::BeginPopup(menuId)) {
            ImGui::TextDisabled("%s", f.name->c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Rename")) {
                view.isRenaming = true;
                view.renameTrackIndex = uid;
            }
            const bool isMain = f.trackId != nullptr &&
                                *f.trackId == document::kMainBusId;
            if (ImGui::MenuItem("Hide Track", nullptr, false, !isMain)) {
                if (f.trackId != nullptr) {
                    view.requestHideTrackId = *f.trackId;
                }
            }
            if (isMain &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Main is where everything ends up.");
            }
            size_t hiddenCount = 0;
            for (const auto& t : edit.tracks()) {
                if (t.hidden) ++hiddenCount;
            }
            if (ImGui::MenuItem("Show All Tracks", nullptr, false,
                                hiddenCount > 0)) {
                view.requestShowAllTracks = true;
            }
            if (hiddenCount > 0) {
                ImGui::TextDisabled("%zu hidden", hiddenCount);
            }
            ImGui::EndPopup();
        }

        if (overName && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            view.isRenaming = true;
            view.renameTrackIndex = uid;
        }
    }

    // Record/mute/solo, drawn to PTXExtractor's TrackStateIndicator spec.
    // Audio gets a circular record indicator in the 21x17 slot to the LEFT;
    // M/S retain their original pixels. MIDI still has M/S only.
    // Drawn rather than composed from ImGui::Button so the active fill can
    // be the state colour instead of the theme's control gradient.
    {
        const ImVec2 msSize(21.0f, 17.0f);
        const float msGap = 3.0f;
        const float msY = y + g.rowPadding;
        const int toggleCount = f.recordArm != nullptr ? 3 : 2;
        // The strip button shares the toggle row's geometry but is momentary:
        // it opens a panel rather than holding a state the row can show.
        const int slotCount = toggleCount + 1;
        float msX = origin.x + g.gutterWidth -
            (msSize.x * static_cast<float>(slotCount) +
             msGap * static_cast<float>(slotCount - 1)) - 12.0f;
        if (f.trackId != nullptr) {
            ImGui::PushID(uid + 9000);
            ImGui::SetCursorScreenPos(ImVec2(msX, msY));
            const bool pressed = ImGui::InvisibleButton("##strip", msSize);
            const bool hovered = ImGui::IsItemHovered();
            if (pressed) view.requestChannelStripTrackId = *f.trackId;
            const ImVec2 bMin(msX, msY);
            const ImVec2 bMax(msX + msSize.x, msY + msSize.y);
            // Same box, fill and border as the R/M/S toggles beside it, so
            // the row reads as one run of four controls.
            dl->AddRectFilled(bMin, bMax,
                              hovered ? C(pal.surfaceStrong)
                                      : C(pal.trackControlInactive), 3.0f);
            dl->AddRect(bMin, bMax,
                        hovered ? C(pal.borderStrong) : C(pal.border), 3.0f);
            // A compact two-fader glyph sized to this box (the side rail's
            // version is drawn for a larger button and spilled over the
            // border here): two rails with a cap each, at different heights.
            {
                const ImU32 col = C(hovered ? pal.text : pal.textMuted);
                const float cx = (bMin.x + bMax.x) * 0.5f;
                const float top = bMin.y + 3.5f;
                const float bot = bMax.y - 3.5f;
                for (int k = 0; k < 2; ++k) {
                    const float x = cx + (k == 0 ? -3.5f : 3.5f);
                    dl->AddLine(ImVec2(x, top), ImVec2(x, bot), col, 1.0f);
                    const float capY = k == 0 ? top + 3.0f : bot - 4.0f;
                    dl->AddRectFilled(ImVec2(x - 2.0f, capY),
                                      ImVec2(x + 2.0f, capY + 2.5f), col, 1.0f);
                }
            }
            if (hovered) ImGui::SetTooltip("Channel strip");
            ImGui::PopID();
            msX += msSize.x + msGap;
        }
        struct Toggle {
            const char* label;
            bool* flag;
            ImVec4 active;
            const char* tooltip;
        };
        Toggle toggles[3];
        int nextToggle = 0;
        if (f.recordArm != nullptr) {
            toggles[nextToggle++] =
                Toggle{"R", f.recordArm, pal.danger, "Record arm"};
        }
        toggles[nextToggle++] =
            Toggle{"M", f.mute, pal.trackMuteActive, "Mute (Shift+M)"};
        toggles[nextToggle++] =
            Toggle{"S", f.solo, pal.trackSoloActive, "Solo (Shift+S)"};
        ImGui::PushID(uid + 5000);
        for (int b = 0; b < toggleCount; ++b) {
            ImGui::PushID(b);
            ImGui::SetCursorScreenPos(ImVec2(msX, msY));
            const bool pressed = ImGui::InvisibleButton("##ms", msSize);
            const bool hovered = ImGui::IsItemHovered();
            const bool isSolo = toggles[b].flag == f.solo;
            if (pressed) {
                const ImGuiIO& io = ImGui::GetIO();
                // Either modifier: on macOS ImGui swaps them, so testing one
                // alone is wrong on one platform or the other.
                const bool commandHeld = io.KeySuper || io.KeyCtrl;
                if (isSolo && commandHeld && f.trackId != nullptr) {
                    view.requestToggleSoloSafeTrackId = *f.trackId;
                } else if (b == 0 && f.recordArm != nullptr &&
                           view.deferRecordArmRequests && f.trackId != nullptr) {
                    view.requestRecordArmTrackId = *f.trackId;
                } else {
                    *toggles[b].flag = !*toggles[b].flag;
                    edit.notifyChanged();
                }
            }
            const Toggle& t = toggles[b];
            const ImVec2 bMin(msX, msY);
            const ImVec2 bMax(msX + msSize.x, msY + msSize.y);
            const bool on = *t.flag;
            const bool isRecordArm = b == 0 && f.recordArm != nullptr;
            if (isRecordArm) {
                theme::drawRecordArmIndicator(
                    dl, ImVec2((bMin.x + bMax.x) * 0.5f,
                               (bMin.y + bMax.y) * 0.5f),
                    6.5f, on, hovered);
            } else {
                const bool safe = isSolo && f.soloSafe != nullptr &&
                                  *f.soloSafe;
                const ImU32 fill = on ? C(t.active)
                                      : (hovered ? C(pal.surfaceStrong)
                                                 : C(pal.trackControlInactive));
                dl->AddRectFilled(bMin, bMax, fill, 3.0f);
                // Solo-safe is a ring, not a fill: the track is exempt from
                // other solos, which is a standing state — filling it would
                // read as "this track is soloed" and it is not.
                dl->AddRect(bMin, bMax,
                            safe ? C(t.active)
                                 : (on ? C(t.active) : C(pal.border)),
                            3.0f, 0, safe ? 2.0f : 1.0f);
                // Dark text on the bright active fills; both amber and yellow
                // are too light for the normal foreground to stay legible.
                const ImU32 labelCol =
                    on ? IM_COL32(32, 30, 28, 255) : C(pal.textMuted);
                theme::drawCenteredControlLabel(
                    dl,
                    theme::Rect{ImVec2(msX, msY),
                                ImVec2(msX + msSize.x, msY + msSize.y)},
                    labelCol, t.label);
            }
            if (hovered) {
                ImGui::SetTooltip("%s", t.tooltip);
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
    constexpr float controlX = kGutterContentX;
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
        const bool gainDragged =
            ImGui::SliderFloat("##gain", &gainDb, -60.0f, 6.0f, "");
        const bool gainReset = theme::altClickedReset();
        if (gainReset) gainDb = 0.0f;
        const double nextGain = std::pow(10.0f, gainDb / 20.0f);
        if ((gainDragged || gainReset) && *f.gain != nextGain) {
            *f.gain = nextGain;
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
        const bool panDragged =
            ImGui::SliderFloat("##pan", &panVal, -1.0f, 1.0f, "");
        const bool panReset = theme::altClickedReset();
        if (panReset) panVal = 0.0f;
        if ((panDragged || panReset) && *f.pan != panVal) {
            *f.pan = panVal;
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
                      audio::DecodedAudioAssetPtr>& assetBuffers,
                  float trackHeight,
                  float timelineHeight,
                  const TransientSnapshotMap& transientAnalyses,
                  const TrackGainNodes* gainNodes) {
    // Draw directly into the host window's draw list (no child windows — they
    // introduce scrolling/sizing bugs that hid the clips in RB-2's first cut).
    // The gutter width follows the controls it contains; the remaining canvas
    // stays dedicated to clips, waveforms, and timeline interaction.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    view.timelineKeyboardFocus = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows);
    if (view.timelineKeyboardFocus && !ImGui::GetIO().WantTextInput) {
#ifdef __APPLE__
        constexpr bool macOS = true;
#else
        constexpr bool macOS = false;
#endif
        if (ImGui::Shortcut(transientNavigationShortcut(
                macOS, NavigationDirection::Previous, true))) {
            view.requestTransientNavigation = true;
            view.transientNavigationDirection = NavigationDirection::Previous;
            view.requestTransientSelectionExtension = true;
        } else if (ImGui::Shortcut(transientNavigationShortcut(
                       macOS, NavigationDirection::Next, true))) {
            view.requestTransientNavigation = true;
            view.transientNavigationDirection = NavigationDirection::Next;
            view.requestTransientSelectionExtension = true;
        } else if (ImGui::Shortcut(transientNavigationShortcut(
                       macOS, NavigationDirection::Previous, false))) {
            view.requestTransientNavigation = true;
            view.transientNavigationDirection = NavigationDirection::Previous;
            view.requestTransientSelectionExtension = false;
        } else if (ImGui::Shortcut(transientNavigationShortcut(
                       macOS, NavigationDirection::Next, false))) {
            view.requestTransientNavigation = true;
            view.transientNavigationDirection = NavigationDirection::Next;
            view.requestTransientSelectionExtension = false;
        }
    }
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // 260 matches PTXExtractor's default track-header width, which is sized to
    // hold a name plus a full row of per-track controls without truncation.
    const float gutterWidth = 260.0f;
    const float totalWidth = avail.x;
    // Reported outward so every zoom path can preserve the playhead's lane X
    // position (see zoomAroundSample).
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
    // Control+scroll scales the row height (see the wheel handler); the
    // format-derived minimum is still enforced so controls never clip.
    trackHeight = std::max(trackHeight * view.trackHeightScale, minTrackHeight);

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
    // The Edit answers the solo question for every track type at once, so the
    // header dimming below matches exactly what GraphBuilder silences.
    const bool anySoloed = edit.anySoloed();
    // MIDI rows carry one extra control row (the instrument), so they are
    // taller than audio rows. An opened disclosure adds a separate automation
    // lane after the base row; collapsed geometry remains byte-for-byte the
    // same as before automation existed.
    const float midiTrackHeight = trackHeight + compactControlHeight + rowGap;
    constexpr float automationLaneHeight = 72.0f;
    // Height is a property of the row, not of a band: a track carrying an
    // instrument needs the extra control row, everything else does not.
    auto baseHeightOf = [&](const document::Track& t) {
        const float base =
            t.instrument.uidString.empty() ? trackHeight : midiTrackHeight;
        const auto it = view.trackHeightScales.find(t.id);
        const float perTrack = it != view.trackHeightScales.end() ? it->second : 1.0f;
        return std::max(minTrackHeight, base * perTrack);
    };
    // A hidden row is zero-height rather than absent from this table. Every
    // selection, drag and command in the timeline indexes tracks by their
    // position in the document; compacting the table here would silently
    // renumber all of them the moment a track was hidden.
    // The lanes an open track shows, top to bottom. An open track with no
    // explicit list shows one Automation lane, the way it always has.
    auto lanesOf = [&](const std::string& id) -> std::vector<LaneMode> {
        if (!view.expandedTracks.contains(id)) return {};
        const auto it = view.trackLanes.find(id);
        if (it == view.trackLanes.end() || it->second.empty()) {
            return {LaneMode::Automation};
        }
        return it->second;
    };
    // Sends and Inserts lanes grow with their lists: a header row, one row
    // per entry (at least one, for the empty message) and an add row.
    auto laneHeightFor = [&](const document::Track& t, LaneMode mode) -> float {
        if (mode == LaneMode::Automation) return automationLaneHeight;
        const size_t n = mode == LaneMode::Sends ? t.sends.size()
                                                 : t.plugins.size();
        return 30.0f + 24.0f * static_cast<float>(std::max<size_t>(1, n)) + 28.0f;
    };
    auto lanesHeightOf = [&](const document::Track& t) -> float {
        float h = 0.0f;
        for (const LaneMode mode : lanesOf(t.id)) h += laneHeightFor(t, mode);
        return h;
    };
    auto rowOffsets = [&](const auto& channels) {
        std::vector<float> offsets(channels.size() + 1, 0.0f);
        for (size_t index = 0; index < channels.size(); ++index) {
            const float height = channels[index].hidden
                ? 0.0f
                : baseHeightOf(channels[index]) + lanesHeightOf(channels[index]);
            offsets[index + 1] = offsets[index] + height;
        }
        return offsets;
    };
    const auto audioOffsets = rowOffsets(tracks);
    const float audioRegionHeight = audioOffsets.back();
    const float tracksRegionHeight = audioRegionHeight;
    // Only the track rows scroll; the ruler, marker lane and add-track "+" are
    // pinned. Scrolling the rows ourselves (instead of the host window) keeps
    // `origin` fixed, so the header can't slide up and clip against the top —
    // and reserving only the VISIBLE row height below keeps the window itself
    // from ever becoming scrollable, which is what moved `origin` before.
    const float headerBottomY = origin.y + timelineHeight + markerLaneHeight;
    const float visibleTracksHeight =
        std::max(0.0f, origin.y + totalHeight - headerBottomY);
    const float maxVerticalScroll =
        std::max(0.0f, tracksRegionHeight - visibleTracksHeight);
    view.verticalScroll =
        std::clamp(view.verticalScroll, 0.0f, maxVerticalScroll);
    const float trackAreaButtonHeight =
        std::min(tracksRegionHeight, visibleTracksHeight);
    char areaBtn[32];
    std::snprintf(areaBtn, sizeof(areaBtn), "##timeline_area_%p", &view);
    // The InvisibleButton covers the clip lane (right of the gutter) so it
    // doesn't eat clicks meant for the gutter's gain/pan sliders.
    bool areaHovered = false;
    if (tracksRegionHeight > 0.0f) {
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + gutterWidth,
                   origin.y + timelineHeight + markerLaneHeight));
        // This area button spans every track row and every expanded automation
        // lane. Small controls drawn later inside those lanes — the automation
        // tool buttons — can spill right of the gutter and overlap it; without
        // allowing overlap here, this earlier item swallows their clicks (the
        // eraser button, sitting furthest right, was the one that lost).
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(
            areaBtn, ImVec2(totalWidth - gutterWidth, trackAreaButtonHeight));
        areaHovered = ImGui::IsItemHovered();
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Holding Command turns the pointer into a select-only tool: it draws a
    // range and never grabs a clip, so a selection can be made straight over
    // clips without nudging them.
    // "Command as selector": while the platform command modifier is held
    // (Command on macOS, Ctrl elsewhere) a press marks a time range instead of
    // picking up the clip under it. ImGui's macOS behavior swaps Command and
    // Control, so the command modifier surfaces as KeyCtrl on every platform;
    // reading KeySuper (as this once did) fired on the physical Control key
    // instead, which is why holding Command appeared to do nothing.
    const bool selectorMode = ImGui::GetIO().KeyCtrl;

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
    const double bpm = edit.tempoBpm();
    const auto& meterMap = edit.meterMap();
    const auto& tempoMap = edit.tempoMap();
    GridStep grid = gridStepFor(view.tcMode, view.samplesPerPixel, sr,
                                24.0, bpm, &meterMap, &tempoMap);
    // A chosen grid increment overrides the format grid, but only while its
    // lines stay at least a few pixels apart — otherwise a fine increment
    // zoomed out would draw thousands of lines, so the format grid stands in.
    bool gridOverridden = false;
    if (view.gridIncrementSamples > 0 &&
        view.gridIncrementSamples / view.samplesPerPixel >= 4.0) {
        grid.minor = view.gridIncrementSamples;
        grid.major = view.gridIncrementSamples * 5;
        gridOverridden = true;
    }
    // One snap policy serves automation points, seeks, clip moves and range
    // edges. Turning Snap off preserves the raw sample under the pointer.
    auto snapPosition = [&](int64_t pos) -> int64_t {
        pos = std::max<int64_t>(0, pos);
        if (!view.snapEnabled) return pos;
        if (view.snapIncrementSamples > 0) {
            const int64_t inc = view.snapIncrementSamples;
            return ((pos + inc / 2) / inc) * inc;
        }
        return snapSampleToFormat(pos, view.tcMode, view.samplesPerPixel, sr,
                                  24.0, bpm, &meterMap, &tempoMap);
    };

    // Walks the visible grid at `step`. Majors and minors are walked
    // separately rather than one loop testing `sample % major`: at 29.97 fps a
    // whole-frame minor does not divide a whole-second major, and every major
    // tick would go missing.
    auto forEachUniformLine = [&](int64_t step, auto&& fn) {
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

    // Bars are not a fixed number of samples once the meter can change, so
    // bars|beats walks the map instead of stepping. A uniform step drawn under
    // a 3/4 section puts every line in the wrong place — and the ruler labels
    // would disagree with the counter, which reads through the same map.
    const bool musicalGrid =
        view.tcMode == TimecodeMode::BarsBeats && !gridOverridden;
    auto forEachMusicalLine = [&](bool majors, auto&& fn) {
        const double perBar =
            document::samplesPerBarAtBar(1, sr, tempoMap, meterMap);
        if (!(perBar > 0.0)) return;
        // Show every bar until they crowd, then every 2, 4, 8… — the same
        // ladder idea as the uniform grid, counted in bars.
        int barStride = 1;
        while (perBar * barStride / view.samplesPerPixel < 44.0 &&
               barStride < 4096) {
            barStride *= 2;
        }
        const int64_t viewEnd = view.scrollSamples + static_cast<int64_t>(
            (totalWidth - gutterWidth) * view.samplesPerPixel);
        for (int bar = 1;; bar += barStride) {
            const int64_t barSample =
                document::sampleAtBar(bar, sr, tempoMap, meterMap);
            if (barSample > viewEnd) break;
            const auto emit = [&](int64_t sample) {
                if (sample < view.scrollSamples) return;
                const double x = origin.x + gutterWidth +
                    (sample - view.scrollSamples) / view.samplesPerPixel;
                if (x >= origin.x + gutterWidth &&
                    x <= origin.x + totalWidth) {
                    fn(x, sample);
                }
            };
            if (majors) {
                emit(barSample);
            } else if (barStride == 1) {
                // Beats only while every bar is drawn; past that the minors
                // would be closer together than they are readable.
                const auto signature =
                    document::signatureAtBar(meterMap, bar);
                const double perBeat =
                    document::samplesPerBeatAtBar(bar, sr, tempoMap, meterMap);
                for (int beat = 1; beat < signature.numerator; ++beat) {
                    emit(barSample + static_cast<int64_t>(perBeat * beat));
                }
            }
            if (barSample > viewEnd) break;
        }
    };

    auto forEachGridLine = [&](int64_t step, auto&& fn) {
        if (musicalGrid) {
            forEachMusicalLine(step == grid.major, fn);
            return;
        }
        forEachUniformLine(step, fn);
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
        const std::string label =
            formatTimecode(sample, view.tcMode, sr, 24.0, bpm, &meterMap,
                           &tempoMap);
        dl->AddText(rulerFont, rulerFontSize, ImVec2(x + 4.0f, origin.y + 2.0f),
                    C(pal.textMuted), label.c_str());
    });
    dl->PopClipRect();

    // The post-fader meter for one row. Null nodes meter as silence rather
    // than leaving a hole, so the column stays straight while a graph rebuild
    // is in flight.
    auto drawTrackMeter = [&](float top, float height,
                              const std::string& trackId) {
        const float meterTop = top + 4.0f;
        const float meterHeight = height - 8.0f;
        if (meterHeight < 8.0f) return;
        engine::GainNode* node = nullptr;
        if (gainNodes != nullptr) {
            const auto found = gainNodes->find(trackId);
            if (found != gainNodes->end()) node = found->second.get();
        }
        if (drawLevelMeter(node, ImVec2(origin.x + kMeterX, meterTop),
                           meterHeight, view.meterOptions, 2,
                           LevelMeterStyle{},
                           sessionHasFloatHeadroom(edit.bitDepth()))) {
            view.meterOptionsChanged = true;
        }
    };

    // The track's colour band, with its disclosure arrow. Drawn per row by
    // both the audio and MIDI loops so the two bands cannot drift apart.
    //
    // The arrow points right when the track is closed and down when it is
    // open, the direction convention every file browser uses. Open rows reveal
    // a parameter-selectable automation lane below their ordinary content row.
    auto drawTrackBand = [&](float top, float height,
                             const ImVec4& fallbackColor,
                             const std::string& savedColor,
                             const std::string& trackId, int uid) {
        const float bandTop = top + 3.0f;
        const float bandBottom = top + height - 3.0f;
        if (bandBottom - bandTop < 8.0f) return;

        const ImVec2 bandMin(origin.x + kBandX, bandTop);
        const ImVec2 bandMax(origin.x + kBandX + kBandW, bandBottom);

        char bandId[32];
        std::snprintf(bandId, sizeof(bandId), "##trackBand_%d", uid);
        ImGui::SetCursorScreenPos(bandMin);
        ImGui::InvisibleButton(bandId, ImVec2(kBandW, bandBottom - bandTop),
                               ImGuiButtonFlags_MouseButtonLeft |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        const bool expanded = view.expandedTracks.count(trackId) != 0;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (expanded) {
                view.expandedTracks.erase(trackId);
                if (view.revealAutomationOwnerId == trackId) {
                    view.revealAutomationOwnerId.clear();
                }
            } else {
                view.expandedTracks.insert(trackId);
                view.revealAutomationOwnerId = trackId;
            }
        }

        char popupId[40];
        std::snprintf(popupId, sizeof(popupId), "##trackColor_%d", uid);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup(popupId);
        }
        if (hovered) {
            ImGui::SetTooltip(
                "Open automation; right-click for track color");
        }
        std::string selectedColor;
        if (drawTrackColorPopup(popupId, savedColor, selectedColor)) {
            view.requestTrackColorId = trackId;
            view.requestTrackColor = std::move(selectedColor);
        }

        const ImVec4 color = trackColorValue(savedColor, fallbackColor);
        dl->AddRectFilled(bandMin, bandMax, C(color), 3.0f);
        if (hovered) {
            dl->AddRect(bandMin, bandMax, C(pal.text), 3.0f, 0, 1.5f);
        }

        // The arrow is punched in the darkest surface rather than the text
        // colour: the band carries saturated identity colours, and a light
        // glyph on those reads as a smudge.
        const ImU32 arrowCol = C(pal.trackLaneSurface);
        const ImVec2 c((bandMin.x + bandMax.x) * 0.5f,
                       (bandMin.y + bandMax.y) * 0.5f);
        constexpr float kArm = 4.0f;
        if (expanded) {
            dl->AddTriangleFilled(ImVec2(c.x - kArm, c.y - kArm * 0.6f),
                                  ImVec2(c.x + kArm, c.y - kArm * 0.6f),
                                  ImVec2(c.x, c.y + kArm * 0.8f), arrowCol);
        } else {
            dl->AddTriangleFilled(ImVec2(c.x - kArm * 0.6f, c.y - kArm),
                                  ImVec2(c.x - kArm * 0.6f, c.y + kArm),
                                  ImVec2(c.x + kArm * 0.8f, c.y), arrowCol);
        }
    };

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
    bool addTrackMenuRequested = false;
    bool addBusRequested = false;
    if (ImGui::BeginPopupContextItem("##addChannelMenu")) {
        if (ImGui::MenuItem("Add Audio Track")) addTrackMenuRequested = true;
        if (ImGui::MenuItem("Add Bus")) addBusRequested = true;
        ImGui::EndPopup();
    }
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
    const float tracksTop = headerBottomY - view.verticalScroll;
    // Confine every row (gutter headers, clips, automation, the selection) to
    // the area below the pinned header, so a scrolled-up row is clipped under
    // the ruler/marker lane rather than painting over them. Popped just before
    // the playhead, which spans the full height. Balanced with the nested clip
    // rects the clip lanes push inside this region.
    dl->PushClipRect(ImVec2(origin.x, headerBottomY),
                     ImVec2(origin.x + totalWidth, origin.y + totalHeight),
                     true);
    const bool gutterHovered =
        windowHovered &&
        mouse.x >= origin.x && mouse.x <= origin.x + gutterWidth &&
        mouse.y >= tracksTop &&
        mouse.y <= tracksTop + tracksRegionHeight;

    // Where the MIDI band starts: immediately below the audio rows, so the two
    // read as one contiguous stack of tracks rather than two separate lists.

    // One list, so a row index is just an index. selectedTrackIndex used to
    // encode three vectors into one number and decode it back, which is why
    // the MIDI clip menu needed a separate id to survive the round trip.
    auto indexAtY = [](float y, float top,
                       const std::vector<float>& offsets) -> int {
        const float local = y - top;
        if (local < 0.0f || offsets.empty() || local >= offsets.back()) {
            return -1;
        }
        const auto next = std::upper_bound(offsets.begin(), offsets.end(), local);
        const int index = static_cast<int>(next - offsets.begin()) - 1;
        return index >= 0 && index + 1 < static_cast<int>(offsets.size())
            ? index : -1;
    };
    auto rowAtY = [&](float y) -> int {
        return indexAtY(y, tracksTop, audioOffsets);
    };
    // The inverse, for drawing a lane-scoped selection. False when the row no
    // longer exists — a track deleted mid-selection.
    auto rowExtent = [&](int row, float& outY, float& outH) -> bool {
        if (row < 0 || row >= static_cast<int>(tracks.size())) return false;
        outY = tracksTop + audioOffsets[static_cast<size_t>(row)];
        outH = audioOffsets[static_cast<size_t>(row) + 1] -
               audioOffsets[static_cast<size_t>(row)];
        return true;
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

    bool automationMouseOver = false;
    bool automationConsumedClick = false;
    if (view.editingAutomationValue &&
        !view.expandedTracks.contains(view.automationEditOwnerId)) {
        view.editingAutomationValue = false;
        view.focusAutomationValue = false;
        view.automationEditOwnerId.clear();
        view.automationEditPointId.clear();
    }
    if (view.drawingAutomation &&
        !view.expandedTracks.contains(view.drawingAutomationOwnerId)) {
        view.drawingAutomation = false;
        view.drawingAutomationOwnerId.clear();
        view.automationDrawOriginal.clear();
        view.automationDrawStroke.clear();
        view.automationDrawFlipped = false;
                view.automationSteepnessLatched = false;
    }
    const bool automationEditorOpenAtFrameStart =
        view.editingAutomationValue;
    // Cancel any in-flight automation gesture on this owner. Called when the
    // lane's content changes out from under an active drag/edit/draw.
    auto cancelAutomationGesture = [&](const std::string& ownerId) {
        if (view.automationOwnerId == ownerId) {
            view.draggingAutomation = false;
            view.automationOwnerId.clear();
            view.automationPointId.clear();
        }
        if (view.automationEditOwnerId == ownerId) {
            view.editingAutomationValue = false;
            view.automationEditOwnerId.clear();
            view.automationEditPointId.clear();
        }
        if (view.drawingAutomationOwnerId == ownerId) {
            view.drawingAutomation = false;
            view.drawingAutomationOwnerId.clear();
            view.automationDrawOriginal.clear();
            view.automationDrawStroke.clear();
            view.automationDrawFlipped = false;
            view.automationSteepnessLatched = false;
        }
    };
    // The lane's content selector, drawn at the top-left of every expanded
    // lane regardless of what it shows. One combo carries both the automation
    // parameter (Volume/Pan) and the higher-level lane mode (Sends/Inserts),
    // because they are mutually exclusive views of the same lane and a second
    // control would not fit beside the automation tools.
    // `laneIndex` is which of the track's lanes this header belongs to; -1
    // means "the Automation lane, wherever it sits" (the automation renderer
    // does not know its own index). Below the combo: "+" adds a lane, "×"
    // removes this one when there is more than one.
    auto drawLaneModeCombo = [&](const std::string& ownerId, float top,
                                 int laneIndex = -1) {
        std::vector<LaneMode> lanes = lanesOf(ownerId);
        if (lanes.empty()) lanes = {LaneMode::Automation};
        if (laneIndex < 0) {
            laneIndex = 0;
            for (size_t i = 0; i < lanes.size(); ++i) {
                if (lanes[i] == LaneMode::Automation) {
                    laneIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        if (laneIndex >= static_cast<int>(lanes.size())) return;
        const LaneMode mode = lanes[static_cast<size_t>(laneIndex)];
        bool otherAutomation = false;
        for (size_t i = 0; i < lanes.size(); ++i) {
            if (static_cast<int>(i) != laneIndex &&
                lanes[i] == LaneMode::Automation) {
                otherAutomation = true;
            }
        }
        const AutomationParameter parameter =
            view.automationParameters.count(ownerId)
                ? view.automationParameters[ownerId]
                : AutomationParameter::Volume;
        const char* label =
            mode == LaneMode::Sends      ? "Sends"
            : mode == LaneMode::Inserts  ? "Inserts"
            : parameter == AutomationParameter::Pan ? "Pan"
                                                    : "Volume";
        const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + kGutterContentX - 4.0f, top + 5.0f));
        ImGui::PushID(ownerId.c_str());
        ImGui::PushID(laneIndex);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::BeginCombo("##laneMode", label,
                              ImGuiComboFlags_HeightSmall)) {
            struct Entry { const char* name; LaneMode mode;
                           AutomationParameter param; };
            const Entry entries[] = {
                {"Volume", LaneMode::Automation, AutomationParameter::Volume},
                {"Pan", LaneMode::Automation, AutomationParameter::Pan},
                {"Sends", LaneMode::Sends, AutomationParameter::Volume},
                {"Inserts", LaneMode::Inserts, AutomationParameter::Volume},
            };
            for (const auto& entry : entries) {
                const bool selected =
                    entry.mode == mode &&
                    (entry.mode != LaneMode::Automation ||
                     entry.param == parameter);
                // One automation lane per track: its drag/edit state is
                // keyed by the track, so a second would fight the first.
                const bool blocked =
                    entry.mode == LaneMode::Automation && otherAutomation;
                if (ImGui::Selectable(entry.name, selected,
                                      blocked ? ImGuiSelectableFlags_Disabled
                                              : ImGuiSelectableFlags_None)) {
                    lanes[static_cast<size_t>(laneIndex)] = entry.mode;
                    view.trackLanes[ownerId] = lanes;
                    if (entry.mode == LaneMode::Automation) {
                        view.automationParameters[ownerId] = entry.param;
                    }
                    cancelAutomationGesture(ownerId);
                    automationConsumedClick = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("Lane content");

        // Add / remove lane, tucked under the combo in the gutter.
        const ImVec2 small(20.0f, 18.0f);
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + kGutterContentX - 4.0f, top + 31.0f));
        if (ImGui::Button("+", small)) {
            // The first kind this track does not show yet, Sends first.
            LaneMode next = LaneMode::Sends;
            auto has = [&](LaneMode m) {
                return std::find(lanes.begin(), lanes.end(), m) != lanes.end();
            };
            if (has(LaneMode::Sends)) next = LaneMode::Inserts;
            if (has(LaneMode::Sends) && has(LaneMode::Inserts)) {
                next = LaneMode::Sends;
            }
            lanes.insert(lanes.begin() + laneIndex + 1, next);
            view.trackLanes[ownerId] = lanes;
            automationConsumedClick = true;
        }
        hovered = hovered || ImGui::IsItemHovered();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a lane below");
        if (lanes.size() > 1) {
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("\xc3\x97", small)) {
                if (mode == LaneMode::Automation) cancelAutomationGesture(ownerId);
                lanes.erase(lanes.begin() + laneIndex);
                view.trackLanes[ownerId] = lanes;
                automationConsumedClick = true;
            }
            hovered = hovered || ImGui::IsItemHovered();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this lane");
        }
        automationMouseOver = automationMouseOver || hovered;
        ImGui::PopID();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(savedCursor);
    };
    auto drawAutomationLane = [&](
        const std::string& ownerId,
        const std::vector<document::VolumeAutomationPoint>& storedPoints,
        float top, AutomationParameter parameter) {
        const bool isPan = parameter == AutomationParameter::Pan;
        // The eraser brush half-width, in pixels. Points whose x falls inside
        // the swept band are removed; 9 px is wide enough to catch a point
        // without demanding pixel-accurate aim.
        constexpr float eraseRadiusPx = 9.0f;
        const float bottom = top + automationLaneHeight;
        const float laneLeft = origin.x + gutterWidth;
        const float laneRight = origin.x + totalWidth;
        constexpr float verticalPadding = 8.0f;
        const float graphTop = top + verticalPadding;
        const float graphBottom = bottom - verticalPadding;

        dl->AddRectFilled(ImVec2(origin.x, top),
                          ImVec2(laneLeft, bottom), C(pal.surfaceBase));
        dl->AddRectFilled(ImVec2(laneLeft, top),
                          ImVec2(laneRight, bottom), C(pal.trackLaneAlt));
        dl->AddLine(ImVec2(origin.x, top), ImVec2(laneRight, top),
                    C(pal.border));
        dl->AddLine(ImVec2(origin.x, bottom), ImVec2(laneRight, bottom),
                    C(pal.border));
        drawLaneModeCombo(ownerId, top);

        const ImVec2 toolSize(24.0f, 22.0f);
        constexpr float toolGap = 4.0f;
        // Derived from where the parameter combo ends, not a fixed pixel:
        // the combo starts at the shared gutter-content origin, so a hardcoded
        // value here silently overlaps it the moment that origin moves.
        constexpr float kParameterComboWidth = 90.0f;
        const float toolsLeft =
            origin.x + kGutterContentX - 4.0f + kParameterComboWidth + 8.0f;
        auto drawToolButton = [&](const char* id, AutomationTool tool,
                                  float left, const char* tooltip) {
            const ImVec2 buttonMin(left, top + 5.0f);
            const ImVec2 buttonMax(buttonMin.x + toolSize.x,
                                   buttonMin.y + toolSize.y);
            ImGui::SetCursorScreenPos(buttonMin);
            ImGui::InvisibleButton(id, toolSize);
            const bool hovered = ImGui::IsItemHovered();
            const bool selected = view.automationTool == tool;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                view.automationTool = tool;
                view.draggingAutomation = false;
                view.automationOwnerId.clear();
                view.automationPointId.clear();
                view.drawingAutomation = false;
                view.drawingAutomationOwnerId.clear();
                view.automationDrawOriginal.clear();
                view.automationDrawStroke.clear();
                view.automationDrawFlipped = false;
                view.automationSteepnessLatched = false;
                automationConsumedClick = true;
            }
            const ImU32 buttonFill = C(selected ? pal.surfaceStrong
                                                : pal.trackControlInactive);
            dl->AddRectFilled(buttonMin, buttonMax, buttonFill, 3.0f);
            dl->AddRect(buttonMin, buttonMax,
                        C(selected ? pal.accentStrong
                                   : hovered ? pal.borderStrong : pal.border),
                        3.0f, 0, selected ? 1.5f : 1.0f);
            const ImU32 iconColor = C(selected ? pal.primaryText
                                               : pal.textMuted);
            if (tool == AutomationTool::Pencil) {
                drawPencilGlyph(
                    dl, ImVec2(buttonMin.x + 3.2f, buttonMin.y + 20.6f));
            } else if (tool == AutomationTool::Curve) {
                constexpr int curveSegments = 10;
                ImVec2 curve[curveSegments + 1];
                for (int segment = 0; segment <= curveSegments; ++segment) {
                    const float t = static_cast<float>(segment) /
                                    static_cast<float>(curveSegments);
                    curve[segment] = ImVec2(
                        buttonMin.x + 5.0f + 14.0f * t,
                        buttonMin.y + 16.0f - 10.0f * t * t);
                }
                dl->AddPolyline(curve, curveSegments + 1, iconColor,
                                ImDrawFlags_None, 1.8f);
                dl->AddCircleFilled(curve[0], 1.8f, iconColor);
                dl->AddCircleFilled(curve[curveSegments], 1.8f, iconColor);
            } else if (tool == AutomationTool::Line) {
                const ImVec2 a(buttonMin.x + 6.0f, buttonMin.y + 16.0f);
                const ImVec2 b(buttonMin.x + 18.0f, buttonMin.y + 6.0f);
                dl->AddLine(a, b, iconColor, 1.8f);
                dl->AddCircleFilled(a, 2.0f, iconColor);
                dl->AddCircleFilled(b, 2.0f, iconColor);
            } else {
                // Eraser: a filled rubber block tilted on the page with a
                // band across it separating the sleeve from the tip. Built
                // around the button centre from a long/short axis so its
                // centroid IS the centre — the earlier hand-placed quad and
                // its shavings sat low and left of centre.
                const ImVec2 mid(buttonMin.x + toolSize.x * 0.5f,
                                 buttonMin.y + toolSize.y * 0.5f);
                const ImVec2 lng(0.85f, -0.53f);   // long axis, up-right
                const ImVec2 sh(0.53f, 0.85f);     // short axis, perpendicular
                constexpr float halfLen = 7.6f;
                constexpr float halfWid = 4.4f;
                auto corner = [&](float u, float v) {
                    return ImVec2(mid.x + u * halfLen * lng.x + v * halfWid * sh.x,
                                  mid.y + u * halfLen * lng.y + v * halfWid * sh.y);
                };
                const ImVec2 c1 = corner(+1.0f, +1.0f);
                const ImVec2 c2 = corner(+1.0f, -1.0f);
                const ImVec2 c3 = corner(-1.0f, -1.0f);
                const ImVec2 c4 = corner(-1.0f, +1.0f);
                dl->AddQuadFilled(c1, c2, c3, c4, iconColor);
                // The groove: a chord across the block a third of the way in
                // from the tip, drawn in the button fill so it reads as a seam.
                auto lerp = [](ImVec2 p, ImVec2 q, float t) {
                    return ImVec2(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t);
                };
                dl->AddLine(lerp(c1, c4, 0.4f), lerp(c2, c3, 0.4f),
                            buttonFill, 1.6f);
            }
            automationMouseOver = automationMouseOver || hovered;
            if (hovered) ImGui::SetTooltip("%s", tooltip);
        };
        ImGui::PushID("automationTools");
        drawToolButton("##pencil", AutomationTool::Pencil, toolsLeft,
                       "Pencil: drag to draw automation");
        drawToolButton("##line", AutomationTool::Line,
                       toolsLeft + toolSize.x + toolGap,
                       "Line: drag a straight automation ramp");
        drawToolButton("##curve", AutomationTool::Curve,
                       toolsLeft + 2.0f * (toolSize.x + toolGap),
                       "Curve: drag a ramp\n"
                       "Option (Alt): flip it\n"
                       "Shift or Control: hold the end still and sweep how "
                       "steep it is");
        drawToolButton("##eraser", AutomationTool::Eraser,
                       toolsLeft + 3.0f * (toolSize.x + toolGap),
                       "Eraser: drag across the lane to remove points");
        ImGui::PopID();

        const double playheadValue = document::volumeAutomationDbAt(
            storedPoints, transport.position());
        char readout[24];
        if (isPan) {
            const std::string panText = theme::formatPan(playheadValue);
            std::snprintf(readout, sizeof(readout), "%s", panText.c_str());
        } else {
            std::snprintf(readout, sizeof(readout), "%+.1f dB", playheadValue);
        }
        dl->AddText(ImVec2(origin.x + kGutterContentX, top + 32.0f),
                    C(pal.textSubtle), readout);

        auto clampLaneValue = [&](double value) {
            return isPan ? document::clampPanAutomation(value)
                         : document::clampVolumeAutomationDb(value);
        };
        const double minimumValue = isPan
            ? -1.0 : document::kMinVolumeAutomationDb;
        const double maximumValue = isPan
            ? 1.0 : document::kMaxVolumeAutomationDb;
        auto valueToY = [&](double value) {
            const double clamped = clampLaneValue(value);
            const double normalized =
                (maximumValue - clamped) / (maximumValue - minimumValue);
            return graphTop + static_cast<float>(normalized) *
                                  (graphBottom - graphTop);
        };
        auto yToValue = [&](float y) {
            const double normalized = std::clamp(
                static_cast<double>((y - graphTop) /
                                    std::max(1.0f, graphBottom - graphTop)),
                0.0, 1.0);
            return maximumValue - normalized *
                (maximumValue - minimumValue);
        };
        auto xToSample = [&](float x) {
            const double local = std::clamp(x, laneLeft, laneRight) - laneLeft;
            return snapPosition(static_cast<int64_t>(
                view.scrollSamples + local * view.samplesPerPixel));
        };
        auto sampleToX = [&](int64_t sample) {
            return static_cast<float>(laneLeft +
                (sample - view.scrollSamples) / view.samplesPerPixel);
        };
        auto clearDrawingGesture = [&] {
            view.drawingAutomation = false;
            view.drawingAutomationOwnerId.clear();
            view.automationDrawOriginal.clear();
            view.automationDrawStroke.clear();
            view.automationDrawFlipped = false;
                view.automationSteepnessLatched = false;
        };
        auto upsertStrokePoint = [&](int64_t sample, double value) {
            auto& stroke = view.automationDrawStroke;
            const auto at = std::lower_bound(
                stroke.begin(), stroke.end(), sample,
                [](const auto& point, int64_t position) {
                    return point.sample < position;
                });
            if (at != stroke.end() && at->sample == sample) {
                at->db = clampLaneValue(value);
                return;
            }
            stroke.insert(at, document::VolumeAutomationPoint{
                                  {}, sample, clampLaneValue(value)});
        };
        auto updateDrawingGesture = [&] {
            if (!view.drawingAutomation ||
                view.drawingAutomationOwnerId != ownerId ||
                view.drawingAutomationParameter != parameter) {
                return;
            }
            if (view.drawingAutomationTool == AutomationTool::Eraser) {
                // No snapping: the band should track the pointer exactly so a
                // point is erased the instant the brush covers it.
                const int64_t radius = static_cast<int64_t>(
                    view.samplesPerPixel * eraseRadiusPx);
                const int64_t here = static_cast<int64_t>(
                    view.scrollSamples +
                    (std::clamp(mouse.x, laneLeft, laneRight) - laneLeft) *
                        view.samplesPerPixel);
                view.automationEraseMinSample =
                    std::min(view.automationEraseMinSample, here - radius);
                view.automationEraseMaxSample =
                    std::max(view.automationEraseMaxSample, here + radius);
                return;
            }
            if (view.drawingAutomationTool == AutomationTool::Line) {
                view.automationDrawStroke.clear();
                upsertStrokePoint(view.automationDrawAnchor.sample,
                                  view.automationDrawAnchor.db);
                upsertStrokePoint(xToSample(mouse.x), yToValue(mouse.y));
            } else if (view.drawingAutomationTool == AutomationTool::Curve) {
                // Option flips the curve — and only Option. Control used to
                // mirror it too, by a different route, which made the two
                // modifiers do the same visible thing.
                view.automationDrawFlipped = ImGui::GetIO().KeyAlt;
                // Shift as well as Control, because on macOS Control+left is
                // not reliably a drag at all: ImGui converts a left press made
                // while Control is held into a RIGHT click, to match the OS
                // convention (see AddMouseButtonEvent). Pressing Control after
                // the drag has started works; pressing it first never begins
                // one. Shift is not hijacked by anything and always does.
#if defined(__APPLE__)
                // ImGui's macOS behavior swaps Command and Control internally:
                // physical Control is exposed as KeySuper here.
                const bool adjustSteepness =
                    ImGui::GetIO().KeySuper || ImGui::GetIO().KeyShift;
#else
                const bool adjustSteepness =
                    ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
#endif
                if (adjustSteepness) {
                    if (!view.automationSteepnessLatched) {
                        // Latch where the curve currently ends. Without this
                        // the far end would jump to wherever the pointer had
                        // drifted the moment Control was released.
                        view.automationSteepnessLatched = true;
                        view.automationSteepnessAnchorX = mouse.x;
                        view.automationSteepnessAtLatch =
                            view.automationDrawSteepness;
                        view.automationFrozenSample = xToSample(mouse.x);
                        view.automationFrozenValue = yToValue(mouse.y);
                    }
                    view.automationDrawSteepness = automationSteepnessForDrag(
                        view.automationSteepnessAtLatch,
                        mouse.x - view.automationSteepnessAnchorX);
                } else {
                    view.automationSteepnessLatched = false;
                }

                view.automationDrawStroke.clear();
                const float anchorX = sampleToX(
                    view.automationDrawAnchor.sample);
                // While steepness is being swept the end stays put, so the
                // drag changes the shape and nothing else.
                const int64_t endSample = view.automationSteepnessLatched
                    ? view.automationFrozenSample : xToSample(mouse.x);
                const double endValue = view.automationSteepnessLatched
                    ? view.automationFrozenValue : yToValue(mouse.y);
                const float endX = sampleToX(endSample);
                const float dx = endX - anchorX;
                constexpr float pointSpacing = 6.0f;
                const int steps = std::max(
                    1, static_cast<int>(std::ceil(
                           std::fabs(dx) / pointSpacing)));
                const double startValue = view.automationDrawAnchor.db;
                for (int step = 0; step <= steps; ++step) {
                    const double t = static_cast<double>(step) /
                                     static_cast<double>(steps);
                    const double shaped = automationCurveShape(
                        t, view.automationDrawSteepness,
                        view.automationDrawFlipped);
                    const float x = anchorX + dx * static_cast<float>(t);
                    const double value = startValue +
                        (endValue - startValue) * shaped;
                    upsertStrokePoint(xToSample(x), value);
                }
            } else {
                const float dx = mouse.x - view.automationDrawLastX;
                const float dy = mouse.y - view.automationDrawLastY;
                constexpr float pointSpacing = 6.0f;
                const int steps = std::max(
                    1, static_cast<int>(std::ceil(
                           std::hypot(dx, dy) / pointSpacing)));
                for (int step = 1; step <= steps; ++step) {
                    const float amount = static_cast<float>(step) /
                                         static_cast<float>(steps);
                    const float x = view.automationDrawLastX + dx * amount;
                    const float y = view.automationDrawLastY + dy * amount;
                    upsertStrokePoint(xToSample(x), yToValue(y));
                }
            }
            view.automationDrawLastX = mouse.x;
            view.automationDrawLastY = mouse.y;
        };
        auto envelopeWithStroke = [&] {
            auto result = view.automationDrawOriginal;
            if (view.automationDrawStroke.empty()) return result;
            const int64_t first = view.automationDrawStroke.front().sample;
            const int64_t last = view.automationDrawStroke.back().sample;
            // Preserve the incoming envelope through the sample immediately
            // before the stroke. Without this guard, linear interpolation from
            // the previous breakpoint to the first drawn value would pull the
            // untouched line away from its original level.
            if (first > 0) {
                const int64_t guardSample = first - 1;
                const bool alreadyGuarded = std::any_of(
                    view.automationDrawOriginal.begin(),
                    view.automationDrawOriginal.end(),
                    [&](const auto& point) {
                        return point.sample == guardSample;
                    });
                if (!alreadyGuarded) {
                    result.push_back(document::VolumeAutomationPoint{
                        {}, guardSample,
                        document::volumeAutomationDbAt(
                            view.automationDrawOriginal, guardSample)});
                }
            }
            result.erase(std::remove_if(result.begin(), result.end(),
                                        [&](const auto& point) {
                                            return point.sample >= first &&
                                                   point.sample <= last;
                                        }),
                         result.end());
            for (auto point : view.automationDrawStroke) {
                const auto existing = std::find_if(
                    view.automationDrawOriginal.begin(),
                    view.automationDrawOriginal.end(), [&](const auto& before) {
                        return before.sample == point.sample;
                    });
                if (existing != view.automationDrawOriginal.end()) {
                    point.id = existing->id;
                }
                result.push_back(std::move(point));
            }
            std::sort(result.begin(), result.end(), [](const auto& a,
                                                       const auto& b) {
                return a.sample < b.sample;
            });
            return result;
        };
        auto envelopeWithErase = [&] {
            return automationErase(view.automationDrawOriginal,
                                   view.automationEraseMinSample,
                                   view.automationEraseMaxSample);
        };
        auto replaceEnvelope = [&](
            const std::vector<document::VolumeAutomationPoint>& points) {
            if (isPan) {
                std::vector<document::PanAutomationPoint> panPoints;
                panPoints.reserve(points.size());
                for (const auto& point : points) {
                    panPoints.push_back(
                        {point.id, point.sample, point.db});
                }
                undo.execute(std::make_unique<
                    editing::ReplacePanAutomationCommand>(
                    ownerId, std::move(panPoints)));
            } else {
                undo.execute(std::make_unique<
                    editing::ReplaceVolumeAutomationCommand>(ownerId, points));
            }
        };
        auto movePoint = [&](const document::VolumeAutomationPoint& point) {
            if (isPan) {
                document::PanAutomationPoint panPoint;
                panPoint.id = point.id;
                panPoint.sample = point.sample;
                panPoint.pan = point.db;
                undo.execute(std::make_unique<
                    editing::MovePanAutomationPointCommand>(
                    ownerId, std::move(panPoint)));
            } else {
                undo.execute(std::make_unique<
                    editing::MoveVolumeAutomationPointCommand>(
                    ownerId, point));
            }
        };
        auto removePoint = [&](const std::string& pointId) {
            if (isPan) {
                undo.execute(std::make_unique<
                    editing::RemovePanAutomationPointCommand>(
                    ownerId, pointId));
            } else {
                undo.execute(std::make_unique<
                    editing::RemoveVolumeAutomationPointCommand>(
                    ownerId, pointId));
            }
        };

        if (isPan) {
            for (double guide : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
                const float guideY = valueToY(guide);
                dl->AddLine(ImVec2(laneLeft, guideY),
                            ImVec2(laneRight, guideY),
                            guide == 0.0 ? C(pal.borderStrong)
                                         : C(pal.border));
            }
        } else {
            for (double guide : {0.0, -12.0, -24.0, -48.0}) {
                const float guideY = valueToY(guide);
                dl->AddLine(ImVec2(laneLeft, guideY),
                            ImVec2(laneRight, guideY),
                            guide == 0.0 ? C(pal.borderStrong)
                                         : C(pal.border));
            }
        }

        if (view.drawingAutomation &&
            view.drawingAutomationOwnerId == ownerId &&
            view.drawingAutomationParameter == parameter &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            updateDrawingGesture();
        }

        std::vector<document::VolumeAutomationPoint> previewPoints;
        const auto* drawPoints = &storedPoints;
        if (view.draggingAutomation &&
            view.automationOwnerId == ownerId &&
            view.activeAutomationParameter == parameter) {
            previewPoints = storedPoints;
            const auto point = std::find_if(
                previewPoints.begin(), previewPoints.end(), [&](const auto& p) {
                    return p.id == view.automationPointId;
                });
            if (point != previewPoints.end()) {
                *point = view.automationPreview;
                std::sort(previewPoints.begin(), previewPoints.end(),
                          [](const auto& a, const auto& b) {
                              return a.sample < b.sample;
                          });
                drawPoints = &previewPoints;
            }
        } else if (view.drawingAutomation &&
                   view.drawingAutomationOwnerId == ownerId &&
                   view.drawingAutomationParameter == parameter) {
            previewPoints =
                view.drawingAutomationTool == AutomationTool::Eraser
                    ? envelopeWithErase()
                    : envelopeWithStroke();
            drawPoints = &previewPoints;
        }

        const int64_t visibleStart = std::max<int64_t>(
            0, static_cast<int64_t>(view.scrollSamples));
        const int64_t visibleEnd = std::max<int64_t>(
            visibleStart,
            static_cast<int64_t>(view.scrollSamples +
                (laneRight - laneLeft) * view.samplesPerPixel));
        ImVec2 previous(laneLeft,
                        valueToY(document::volumeAutomationDbAt(
                            *drawPoints, visibleStart)));
        for (const auto& point : *drawPoints) {
            const float x = sampleToX(point.sample);
            if (x <= laneLeft) continue;
            if (x >= laneRight) break;
            const ImVec2 current(x, valueToY(point.db));
            dl->AddLine(previous, current, C(pal.accentStrong), 1.8f);
            previous = current;
        }
        dl->AddLine(previous,
                    ImVec2(laneRight,
                           valueToY(document::volumeAutomationDbAt(
                               *drawPoints, visibleEnd))),
                    C(pal.accentStrong), 1.8f);

        if (view.revealAutomationOwnerId == ownerId) {
            const ImVec2 windowPos = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            const float visibleBottom =
                windowPos.y + windowSize.y - ImGui::GetStyle().WindowPadding.y;
            if (bottom > visibleBottom) {
                ImGui::SetScrollFromPosY(bottom - windowPos.y, 1.0f);
            }
            view.revealAutomationOwnerId.clear();
        }

        std::string hoveredPointId;
        for (const auto& point : *drawPoints) {
            const ImVec2 center(sampleToX(point.sample), valueToY(point.db));
            if (center.x < laneLeft - 6.0f || center.x > laneRight + 6.0f) {
                continue;
            }
            const bool pointHovered =
                std::fabs(mouse.x - center.x) <= 7.0f &&
                std::fabs(mouse.y - center.y) <= 7.0f;
            dl->AddCircleFilled(center, pointHovered ? 5.0f : 4.0f,
                                C(pointHovered ? pal.primaryText
                                               : pal.accentStrong));
            dl->AddCircle(center, pointHovered ? 5.0f : 4.0f,
                          C(pal.accentDeep), 0, 1.0f);
            if (pointHovered) hoveredPointId = point.id;
        }

        if (view.drawingAutomation &&
            view.drawingAutomationOwnerId == ownerId &&
            view.drawingAutomationParameter == parameter &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            !view.automationDrawStroke.empty()) {
            const int64_t latestSample = xToSample(mouse.x);
            const double latestValue = clampLaneValue(yToValue(mouse.y));
            const ImVec2 latestPoint(sampleToX(latestSample),
                                     valueToY(latestValue));
            const std::string valueText =
                formatAutomationDrawValue(parameter, latestValue);
            const ImVec2 textSize = ImGui::CalcTextSize(valueText.c_str());
            constexpr float badgePaddingX = 5.0f;
            constexpr float badgePaddingY = 3.0f;
            constexpr float badgeGap = 8.0f;
            const ImVec2 badgeSize(textSize.x + badgePaddingX * 2.0f,
                                   textSize.y + badgePaddingY * 2.0f);
            ImVec2 badgeMin(latestPoint.x + badgeGap,
                            latestPoint.y - badgeSize.y - badgeGap);
            if (badgeMin.x + badgeSize.x > laneRight) {
                badgeMin.x = latestPoint.x - badgeSize.x - badgeGap;
            }
            badgeMin.x = std::clamp(badgeMin.x, laneLeft,
                                    laneRight - badgeSize.x);
            badgeMin.y = std::clamp(badgeMin.y, graphTop,
                                    graphBottom - badgeSize.y);
            const ImVec2 badgeMax(badgeMin.x + badgeSize.x,
                                  badgeMin.y + badgeSize.y);
            dl->AddRectFilled(badgeMin, badgeMax, C(pal.surfaceStrong), 3.0f);
            dl->AddRect(badgeMin, badgeMax, C(pal.borderStrong), 3.0f);
            dl->AddText(ImVec2(badgeMin.x + badgePaddingX,
                               badgeMin.y + badgePaddingY),
                        C(pal.primaryText), valueText.c_str());
        }

        const bool laneHovered = windowHeldOrHovered &&
            mouse.x >= laneLeft && mouse.x <= laneRight &&
            mouse.y >= top && mouse.y <= bottom;
        automationMouseOver = automationMouseOver || laneHovered;
        if (laneHovered && !hoveredPointId.empty()) {
            if (view.automationTool == AutomationTool::Line) {
                ImGui::SetTooltip(
                    "Drag a straight ramp; double-click to enter a value; "
                    "right-click to delete");
            } else if (view.automationTool == AutomationTool::Curve) {
                ImGui::SetTooltip(
                    "Drag a parabolic ramp; hold Option (Alt on Windows) "
                    "for logarithmic; hold Control to reverse the slope; "
                    "double-click to enter a value; right-click to delete");
            } else if (view.automationTool == AutomationTool::Eraser) {
                ImGui::SetTooltip(
                    "Drag across the lane to erase points; "
                    "right-click to delete this one");
            } else {
                ImGui::SetTooltip(isPan
                    ? "Drag to move; double-click to enter pan; right-click to delete"
                    : "Drag to move; double-click to enter dB; right-click to delete");
            }
        } else if (laneHovered) {
            if (view.automationTool == AutomationTool::Line) {
                ImGui::SetTooltip("Drag a straight automation ramp");
            } else if (view.automationTool == AutomationTool::Curve) {
                ImGui::SetTooltip(
                    "Drag a parabolic automation ramp; hold Option "
                    "(Alt on Windows) for logarithmic; hold Control to "
                    "reverse the slope");
            } else if (view.automationTool == AutomationTool::Eraser) {
                ImGui::SetTooltip("Drag across the lane to erase points");
            } else {
                ImGui::SetTooltip(
                    "Drag to draw automation; click to add one point");
            }
        }

        const bool opensValueEditor =
            laneHovered && !hoveredPointId.empty() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (opensValueEditor) {
            const auto found = std::find_if(
                storedPoints.begin(), storedPoints.end(), [&](const auto& point) {
                    return point.id == hoveredPointId;
                });
            if (found != storedPoints.end()) {
                view.draggingAutomation = false;
                view.activeAutomationParameter = parameter;
                view.automationOwnerId.clear();
                view.automationPointId.clear();
                view.editingAutomationValue = true;
                view.focusAutomationValue = true;
                view.automationEditOwnerId = ownerId;
                view.automationEditPointId = found->id;
                view.automationEditValue = isPan
                    ? found->db * 100.0 : found->db;
                automationConsumedClick = true;
            }
        }

        const bool editingValueAtFrameStart =
            view.editingAutomationValue &&
            view.automationEditOwnerId == ownerId &&
            view.activeAutomationParameter == parameter;
        if (editingValueAtFrameStart) {
            const auto editedPoint = std::find_if(
                storedPoints.begin(), storedPoints.end(), [&](const auto& point) {
                    return point.id == view.automationEditPointId;
                });
            if (editedPoint == storedPoints.end()) {
                view.editingAutomationValue = false;
                view.focusAutomationValue = false;
                view.automationEditOwnerId.clear();
                view.automationEditPointId.clear();
            } else {
                constexpr float editorWidth = 82.0f;
                const float editorHeight = ImGui::GetFrameHeight();
                const ImVec2 pointCenter(sampleToX(editedPoint->sample),
                                         valueToY(editedPoint->db));
                const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(
                    std::clamp(pointCenter.x + 10.0f, laneLeft + 4.0f,
                               laneRight - editorWidth - 4.0f),
                    std::clamp(pointCenter.y - editorHeight * 0.5f,
                               top + 2.0f, bottom - editorHeight - 2.0f)));
                ImGui::PushID(ownerId.c_str());
                ImGui::PushID(view.automationEditPointId.c_str());
                ImGui::SetNextItemWidth(editorWidth);
                if (view.focusAutomationValue) {
                    ImGui::SetKeyboardFocusHere();
                    view.focusAutomationValue = false;
                }
                const bool submitted = ImGui::InputDouble(
                    "##automationValue", &view.automationEditValue,
                    0.0, 0.0, isPan ? "%+.0f" : "%+.1f dB",
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                const bool cancelled = ImGui::IsItemActive() &&
                    ImGui::IsKeyPressed(ImGuiKey_Escape);
                const bool deactivated = ImGui::IsItemDeactivated();
                automationMouseOver = automationMouseOver ||
                    ImGui::IsItemHovered();
                if (ImGui::IsItemHovered()) {
                    if (isPan) {
                        ImGui::SetTooltip(
                            "Pan (-100 = left, 0 = centre, +100 = right)");
                    } else {
                        ImGui::SetTooltip("Volume (%+.1f to %+.1f dB)",
                                          document::kMinVolumeAutomationDb,
                                          document::kMaxVolumeAutomationDb);
                    }
                }
                ImGui::PopID();
                ImGui::PopID();
                ImGui::SetCursorScreenPos(savedCursor);

                if (cancelled) {
                    view.editingAutomationValue = false;
                    view.automationEditOwnerId.clear();
                    view.automationEditPointId.clear();
                    automationConsumedClick = true;
                } else if (submitted || deactivated) {
                    auto updated = *editedPoint;
                    updated.db = clampLaneValue(isPan
                        ? view.automationEditValue / 100.0
                        : view.automationEditValue);
                    if (updated.db != editedPoint->db) {
                        movePoint(updated);
                    }
                    view.editingAutomationValue = false;
                    view.automationEditOwnerId.clear();
                    view.automationEditPointId.clear();
                    automationConsumedClick = true;
                }
            }
        }

        const bool showPencilCursor =
            laneHovered && view.automationTool == AutomationTool::Pencil &&
            !view.editingAutomationValue && !opensValueEditor;
        const bool showEraserBrush =
            laneHovered && view.automationTool == AutomationTool::Eraser &&
            !view.editingAutomationValue;
        if (showEraserBrush) {
            // A hollow ring the width of the erase band, so the reach is
            // visible before committing to a swipe.
            dl->AddCircle(mouse, eraseRadiusPx, C(pal.primaryText), 0, 1.4f);
            dl->AddCircle(mouse, eraseRadiusPx, C(pal.accentDeep), 0, 0.6f);
        }
        if (showPencilCursor) {
            // Two mechanisms on purpose. ImGui's is what a headless test can
            // observe; the view flag is what actually reaches the screen,
            // because the backend's own cursor handling shares one state
            // across viewports and drops the request.
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            view.wantsHiddenCursor = true;
            drawPencilGlyph(ImGui::GetForegroundDrawList(), mouse, 1.1f);
        }

        if (automationEditorOpenAtFrameStart ||
            editingValueAtFrameStart || opensValueEditor) {
            automationConsumedClick = true;
        } else if (!automationConsumedClick &&
                   view.drawingAutomation &&
                   view.drawingAutomationOwnerId == ownerId &&
                   view.drawingAutomationParameter == parameter) {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const auto replacement =
                    view.drawingAutomationTool == AutomationTool::Eraser
                        ? envelopeWithErase()
                        : envelopeWithStroke();
                if (replacement != view.automationDrawOriginal) {
                    replaceEnvelope(replacement);
                }
                clearDrawingGesture();
            }
            automationConsumedClick = true;
        } else if (!automationConsumedClick &&
                   view.draggingAutomation &&
                   view.automationOwnerId == ownerId &&
                   view.activeAutomationParameter == parameter) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                view.automationPreview.sample = xToSample(mouse.x);
                view.automationPreview.db = clampLaneValue(yToValue(mouse.y));
            } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const bool sampleOccupied = std::any_of(
                    storedPoints.begin(), storedPoints.end(),
                    [&](const auto& point) {
                        return point.id != view.automationPointId &&
                               point.sample == view.automationPreview.sample;
                    });
                if (!sampleOccupied &&
                    view.automationPreview != view.automationOriginal) {
                    movePoint(view.automationPreview);
                }
                view.draggingAutomation = false;
                view.automationOwnerId.clear();
                view.automationPointId.clear();
                automationConsumedClick = true;
            }
        } else if (!automationConsumedClick && laneHovered &&
                   ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                   !hoveredPointId.empty()) {
            removePoint(hoveredPointId);
            automationConsumedClick = true;
        } else if (!automationConsumedClick && laneHovered &&
                   ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const auto found = std::find_if(
                storedPoints.begin(), storedPoints.end(), [&](const auto& point) {
                    return point.id == hoveredPointId;
                });
            if (view.automationTool == AutomationTool::Pencil &&
                found != storedPoints.end()) {
                view.draggingAutomation = true;
                view.activeAutomationParameter = parameter;
                view.automationOwnerId = ownerId;
                view.automationPointId = found->id;
                view.automationOriginal = *found;
                view.automationPreview = *found;
            } else {
                view.draggingAutomation = false;
                view.automationOwnerId.clear();
                view.automationPointId.clear();
                view.drawingAutomation = true;
                view.drawingAutomationParameter = parameter;
                view.drawingAutomationTool = view.automationTool;
                view.drawingAutomationOwnerId = ownerId;
                view.automationDrawOriginal = storedPoints;
                view.automationDrawStroke.clear();
                if (view.automationTool == AutomationTool::Eraser) {
                    const int64_t radius = static_cast<int64_t>(
                        view.samplesPerPixel * eraseRadiusPx);
                    const int64_t here = static_cast<int64_t>(
                        view.scrollSamples +
                        (std::clamp(mouse.x, laneLeft, laneRight) - laneLeft) *
                            view.samplesPerPixel);
                    view.automationEraseMinSample = here - radius;
                    view.automationEraseMaxSample = here + radius;
                } else {
                    view.automationDrawFlipped =
                        view.automationTool == AutomationTool::Curve &&
                        ImGui::GetIO().KeyAlt;
                    // Steepness carries over between strokes — it is a tool
                    // setting, like a brush size, not something to relearn
                    // every time. Only the latch resets.
                    view.automationSteepnessLatched = false;
                    view.automationDrawAnchor = {
                        {}, xToSample(mouse.x),
                        clampLaneValue(yToValue(mouse.y))};
                    view.automationDrawLastX = mouse.x;
                    view.automationDrawLastY = mouse.y;
                    upsertStrokePoint(view.automationDrawAnchor.sample,
                                      view.automationDrawAnchor.db);
                }
            }
            automationConsumedClick = true;
        }

    };
    auto drawChannelAutomationLane = [&](
        const std::string& ownerId,
        const std::vector<document::VolumeAutomationPoint>& volumePoints,
        const std::vector<document::PanAutomationPoint>& panPoints,
        float top) {
        const auto [parameterIt, inserted] = view.automationParameters.try_emplace(
            ownerId, AutomationParameter::Volume);
        (void)inserted;
        const AutomationParameter parameter = parameterIt->second;
        if (parameter == AutomationParameter::Volume) {
            drawAutomationLane(ownerId, volumePoints, top, parameter);
            return;
        }

        // The lane renderer is deliberately parameter-agnostic. Normalize pan
        // points into its lightweight UI shape, then convert back only when an
        // undoable document command is committed.
        std::vector<document::VolumeAutomationPoint> normalized;
        normalized.reserve(panPoints.size());
        for (const auto& point : panPoints) {
            normalized.push_back({point.id, point.sample, point.pan});
        }
        drawAutomationLane(ownerId, normalized, top, parameter);
    };

    // Sends / Inserts lane: the track's chain entries as live controls, with
    // the lane-mode combo (and add/remove lane) in the gutter. A send row is
    // target · level · mute · remove; an insert row is name · bypass ·
    // remove; each lane ends in an add button. The same requests and
    // commands the channel strip issues, so the two never disagree.
    auto drawChainLane = [&](const document::Track& track, float top,
                             LaneMode mode, int laneIndex) {
        const float bottom = top + laneHeightFor(track, mode);
        const float laneLeft = origin.x + gutterWidth;
        const float laneRight = origin.x + totalWidth;
        dl->AddRectFilled(ImVec2(origin.x, top),
                          ImVec2(laneLeft, bottom), C(pal.surfaceBase));
        dl->AddRectFilled(ImVec2(laneLeft, top),
                          ImVec2(laneRight, bottom), C(pal.trackLaneAlt));
        dl->AddLine(ImVec2(origin.x, top), ImVec2(laneRight, top),
                    C(pal.border));
        dl->AddLine(ImVec2(origin.x, bottom), ImVec2(laneRight, bottom),
                    C(pal.border));
        drawLaneModeCombo(track.id, top, laneIndex);

        // The whole lane body is a control surface: a press here must never
        // start a range selection or seek underneath the widgets.
        if (mouse.x >= laneLeft && mouse.x <= laneRight && mouse.y >= top &&
            mouse.y <= bottom) {
            automationMouseOver = true;
        }

        const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
        const float x0 = laneLeft + 10.0f;
        float rowY = top + 6.0f;
        constexpr float rowH = 24.0f;
        constexpr float ctrlH = 20.0f;
        const ImU32 mutedCol = C(pal.textMuted);
        // Only the hardware "(unavailable)" suffix depends on the playback
        // channel count, which the timeline does not know; two is the common
        // case and the labels are otherwise identical.
        constexpr int kAssumedPlaybackChannels = 2;
        std::string idScope = track.id + "#lane" + std::to_string(laneIndex);
        ImGui::PushID(idScope.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));

        if (mode == LaneMode::Sends) {
            if (track.sends.empty()) {
                dl->AddText(ImVec2(x0, rowY + 3.0f), mutedCol, "No sends");
                rowY += rowH;
            }
            for (size_t si = 0; si < track.sends.size(); ++si) {
                const auto& send = track.sends[si];
                ImGui::PushID(static_cast<int>(si));
                const bool preFader = document::sendIsPreFader(track, send.id);
                const ImVec4 tapCol = preFader ? pal.clipMidiBorder
                                               : pal.accentStrong;
                dl->AddRectFilled(ImVec2(x0 - 6.0f, rowY + 3.0f),
                                  ImVec2(x0 - 3.0f, rowY + ctrlH - 3.0f),
                                  C(tapCol));
                // Target: click to retarget.
                const std::string label = "\xe2\x86\x92 " +
                    routeTargetLabel(edit, send.target,
                                     kAssumedPlaybackChannels);
                ImGui::SetCursorScreenPos(ImVec2(x0, rowY));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      send.muted ? pal.textMuted : tapCol);
                if (ImGui::Button(label.c_str(), ImVec2(140.0f, ctrlH))) {
                    ImGui::OpenPopup("##laneRetarget");
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(preFader ? "Pre-fader send — click to "
                                                 "retarget"
                                               : "Post-fader send — click to "
                                                 "retarget");
                }
                if (ImGui::BeginPopup("##laneRetarget")) {
                    const auto options = routingTargetOptions(
                        edit, track.id, kAssumedPlaybackChannels, true);
                    for (size_t oi = 0; oi < options.size(); ++oi) {
                        const auto& option = options[oi];
                        ImGui::PushID(static_cast<int>(oi));
                        if (ImGui::Selectable(
                                option.label.c_str(),
                                send.target == option.target,
                                option.enabled
                                    ? ImGuiSelectableFlags_None
                                    : ImGuiSelectableFlags_Disabled)) {
                            RoutingRequest request;
                            request.kind = RoutingRequest::Kind::UpdateSend;
                            request.ownerId = track.id;
                            request.send = send;
                            request.send.target = option.target;
                            view.routing.request(std::move(request));
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndPopup();
                }
                // Level: a plain horizontal fader; Option-click puts it at
                // unity.
                ImGui::SameLine(0.0f, 6.0f);
                float db = send.gain <= 0.0001
                    ? -60.0f
                    : 20.0f * std::log10(static_cast<float>(send.gain));
                ImGui::SetNextItemWidth(150.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 12.0f);
                const bool dragged =
                    ImGui::SliderFloat("##lvl", &db, -60.0f, 6.0f, "%.1f dB");
                ImGui::PopStyleVar();
                const bool reset = theme::altClickedReset();
                if (dragged || reset) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::UpdateSend;
                    request.ownerId = track.id;
                    request.send = send;
                    request.send.gain =
                        reset ? 1.0 : std::pow(10.0, db / 20.0);
                    view.routing.request(std::move(request));
                }
                // Mute and remove.
                ImGui::SameLine(0.0f, 6.0f);
                if (send.muted) {
                    ImGui::PushStyleColor(ImGuiCol_Button, pal.trackMuteActive);
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(32, 30, 28, 255));
                }
                if (ImGui::Button("M", ImVec2(ctrlH, ctrlH))) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::UpdateSend;
                    request.ownerId = track.id;
                    request.send = send;
                    request.send.muted = !send.muted;
                    view.routing.request(std::move(request));
                }
                if (send.muted) ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute send");
                ImGui::SameLine(0.0f, 4.0f);
                if (ImGui::Button("\xc3\x97", ImVec2(ctrlH, ctrlH))) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::RemoveSend;
                    request.ownerId = track.id;
                    request.send.id = send.id;
                    view.routing.request(std::move(request));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove send");
                ImGui::PopID();
                rowY += rowH;
            }
            // Add: pick the destination first, the way the strip does; a bus
            // that does not exist yet can be made on the spot.
            ImGui::SetCursorScreenPos(ImVec2(x0, rowY));
            if (ImGui::Button("+ Send", ImVec2(90.0f, ctrlH))) {
                ImGui::OpenPopup("##laneAddSend");
            }
            if (ImGui::BeginPopup("##laneAddSend")) {
                const auto options = routingTargetOptions(
                    edit, track.id, kAssumedPlaybackChannels, true);
                for (size_t oi = 0; oi < options.size(); ++oi) {
                    const auto& option = options[oi];
                    ImGui::PushID(static_cast<int>(oi));
                    if (!option.enabled) {
                        ImGui::TextDisabled("%s", option.label.c_str());
                    } else if (ImGui::Selectable(option.label.c_str())) {
                        RoutingRequest request;
                        request.kind = RoutingRequest::Kind::AddSend;
                        request.ownerId = track.id;
                        request.send.target = option.target;
                        view.routing.request(std::move(request));
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Selectable("+ New bus\xe2\x80\xa6")) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::AddBusAndSend;
                    request.ownerId = track.id;
                    view.routing.request(std::move(request));
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        } else {  // Inserts
            // Mutations wait until the list has been walked: removing a
            // plugin mid-loop would pull the vector out from under it.
            std::string removeSlotId;
            std::string bypassSlotId;
            bool bypassTo = false;
            if (track.plugins.empty()) {
                dl->AddText(ImVec2(x0, rowY + 3.0f), mutedCol, "No inserts");
                rowY += rowH;
            }
            for (size_t pi = 0; pi < track.plugins.size(); ++pi) {
                const auto& slot = track.plugins[pi];
                ImGui::PushID(static_cast<int>(pi));
                const std::string name =
                    slot.name.empty() ? std::string("(unnamed)") : slot.name;
                ImGui::SetCursorScreenPos(ImVec2(x0, rowY));
                if (slot.bypass) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.textMuted);
                }
                if (ImGui::Button(name.c_str(), ImVec2(180.0f, ctrlH))) {
                    view.requestPluginEditorSlotId = slot.id;
                }
                if (slot.bypass) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Open the plugin's editor");
                }
                ImGui::SameLine(0.0f, 6.0f);
                if (slot.bypass) {
                    ImGui::PushStyleColor(ImGuiCol_Button, pal.trackMuteActive);
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(32, 30, 28, 255));
                }
                if (ImGui::Button("Byp", ImVec2(38.0f, ctrlH))) {
                    bypassSlotId = slot.id;
                    bypassTo = !slot.bypass;
                }
                if (slot.bypass) ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bypass");
                ImGui::SameLine(0.0f, 4.0f);
                if (ImGui::Button("\xc3\x97", ImVec2(ctrlH, ctrlH))) {
                    removeSlotId = slot.id;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove insert");
                ImGui::PopID();
                rowY += rowH;
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, rowY));
            if (ImGui::Button("+ Insert", ImVec2(90.0f, ctrlH))) {
                view.requestPicker = TimelineViewState::PluginPicker::AudioFx;
                view.requestPickerTrackId = track.id;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Add an insert to the end of the chain");
            }
            if (!removeSlotId.empty()) {
                undo.execute(std::make_unique<editing::RemovePluginCommand>(
                    track.id, removeSlotId));
            }
            if (!bypassSlotId.empty()) {
                undo.execute(std::make_unique<editing::SetPluginBypassCommand>(
                    bypassSlotId, bypassTo));
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(savedCursor);
    };

    // Tracks + clips.
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 1.0f));
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const auto& track = tracks[ti];
        // Zero-height, so there is nothing to draw and nothing to hit.
        if (track.hidden) continue;
        float y = tracksTop + audioOffsets[ti];
        if (y > origin.y + totalHeight) break;

        // A 6px grip on the row's bottom edge (in the gutter) resizes the row.
        {
            const float rowTotalH = audioOffsets[ti + 1] - audioOffsets[ti];
            char resizeId[40];
            std::snprintf(resizeId, sizeof(resizeId), "##rowresize_%zu", ti);
            ImGui::SetCursorScreenPos(ImVec2(origin.x, y + rowTotalH - 3.0f));
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton(resizeId, ImVec2(gutterWidth, 6.0f));
            if (ImGui::IsItemHovered() || view.resizingTrackId == track.id) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            if (ImGui::IsItemActive()) {
                view.resizingTrackId = track.id;
                const float base = track.instrument.uidString.empty()
                    ? trackHeight : midiTrackHeight;
                const float laneExtra = lanesHeightOf(track);
                const float desired =
                    std::max(minTrackHeight, (mouse.y - y) - laneExtra);
                view.trackHeightScales[track.id] =
                    std::clamp(desired / base, 0.5f, 4.0f);
            } else if (view.resizingTrackId == track.id) {
                view.resizingTrackId.clear();
            }
        }

        bool selected = view.selectedTrackIndex == static_cast<int>(ti) ||
                        view.selectedTrackIds.count(track.id) > 0;
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
        const ImVec4 trackColor = defaultTrackColor(static_cast<int>(ti));
        drawTrackMeter(y, trackHeight, track.id);
        drawTrackBand(y, trackHeight, trackColor, track.color, track.id,
                      static_cast<int>(ti));

        {
            auto& mutableTrack = const_cast<document::Track&>(track);
            drawTrackGutter(gutter, view, const_cast<document::Edit&>(edit), y,
                            static_cast<int>(ti),
                            TrackGutterFields{&mutableTrack.id,
                                              &mutableTrack.name,
                                              &mutableTrack.gain,
                                              &mutableTrack.pan,
                                              &mutableTrack.mute,
                                              &mutableTrack.solo,
                                              &mutableTrack.soloSafe,
                                              &mutableTrack.recordArm},
                            selected);
        }
        // MIDI content on the same row. A track can hold both — audio clips
        // drawn above, note blobs here — because nothing about the row says
        // which kind it is allowed to carry.
        for (const auto& clip : track.midiClips) {
            if (edit.clipGroupContaining(track.id, clip.id) != nullptr) {
                continue;
            }
            const bool clipIsDragging =
                view.isDragging(TimelineViewState::DragKind::MidiClip) &&
                view.selectedClipId == clip.id;
            // Moved clips are drawn once, on top, at the pointer's row after
            // every track (below), so they cross tracks freely and leave
            // nothing on their own row.
            if (clipIsDragging && !ImGui::GetIO().KeyAlt) continue;
            // A trim previews length and source offset as well as position,
            // and one row carries both clip vectors now — so the preview has
            // to check the dragged clip's kind, not just its id.
            const bool clipIsTrimming = view.isTrimming() &&
                view.dragClipIsMidi && view.selectedClipId == clip.id;
            const int64_t drawStart = (clipIsDragging || clipIsTrimming)
                ? view.dragPreviewStart : clip.timelineStart;
            const int64_t drawLength =
                clipIsTrimming ? view.dragPreviewLength : clip.length;
            const int64_t drawOffset =
                clipIsTrimming ? view.dragPreviewOffset : clip.sourceOffset;
            double clipX = origin.x + gutterWidth +
                (drawStart - view.scrollSamples) / view.samplesPerPixel;
            double clipW = static_cast<double>(drawLength) / view.samplesPerPixel;
            if (clipW < 2) clipW = 2;
            if (clipX + clipW < origin.x + gutterWidth) continue;
            if (clipX > origin.x + totalWidth) continue;

            const ImVec2 clipMin(static_cast<float>(clipX), y + 6.0f);
            const ImVec2 clipMax(static_cast<float>(clipX + clipW),
                                 y + midiTrackHeight - 6.0f);
            const bool isSel = view.selectedClipId == clip.id ||
                               view.selectedClipIds.count(clip.id) > 0;
            dl->PushClipRect(ImVec2(origin.x + gutterWidth, y),
                             ImVec2(origin.x + totalWidth, y + midiTrackHeight),
                             true);
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
                const int64_t winStart = drawOffset;
                const int64_t winEnd = drawOffset + drawLength;
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

            dl->PopClipRect();

            const bool clipHovered =
                mouse.x >= clipMin.x && mouse.x <= clipMax.x &&
                mouse.y >= clipMin.y && mouse.y <= clipMax.y;
            const auto midiGesture =
                clipGestureAt(mouse.x, clipMin.x, clipMax.x);
            if (areaHovered && clipHovered &&
                midiGesture != TimelineViewState::DragKind::None) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (areaHovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                clipHovered) {
                const bool shift = ImGui::GetIO().KeyShift;
                // Command is the selector: it stands down here so the range
                // machinery below marks a span in this lane instead.
                if (!selectorMode) {
                    applyClipSelection(view, edit, static_cast<int>(ti),
                                       clip.id, shift, false);
                    if (!shift) {
                        selectClipRange(view, transport, static_cast<int>(ti),
                                        clip.timelineStart, clip.length);
                        view.dragClipOriginalStart = clip.timelineStart;
                        view.dragPreviewStart = clip.timelineStart;
                        view.dragClipOriginalOffset = clip.sourceOffset;
                        view.dragPreviewOffset = clip.sourceOffset;
                        view.dragClipOriginalLength = clip.length;
                        view.dragPreviewLength = clip.length;
                        view.dragOriginalTrackId = track.id;
                        view.dragClipIsMidi = true;
                        view.dragKind =
                            midiGesture == TimelineViewState::DragKind::None
                                ? TimelineViewState::DragKind::MidiClip
                                : midiGesture;
                    }
                }
            }
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                ImGui::OpenPopup("##midi_clip_ctx");
            }
        }
        // The group clips. Drawn after the members so a group always sits on
        // top of anything that is not in it.
        for (const auto& group : edit.clipGroups()) {
            if (!group.coversTrack(track.id) || group.length <= 0) continue;
            // A nested group is recorded but stood in for by the one above
            // it, the same way a grouped clip is stood in for by its group.
            if (!edit.clipGroupIsTopLevel(group.id)) continue;
            const bool dragging =
                view.isDragging(TimelineViewState::DragKind::AudioClip) &&
                view.selectedClipId == group.id;
            const int64_t drawStart =
                dragging ? view.dragPreviewStart : group.timelineStart;
            double gx = origin.x + gutterWidth +
                (drawStart - view.scrollSamples) / view.samplesPerPixel;
            double gw = static_cast<double>(group.length) / view.samplesPerPixel;
            if (gw < 2) gw = 2;
            if (gx + gw < origin.x + gutterWidth) continue;
            if (gx > origin.x + totalWidth) continue;

            const ImVec2 gMin(static_cast<float>(gx), y + 6.0f);
            const ImVec2 gMax(static_cast<float>(gx + gw),
                              y + baseHeightOf(track) - 6.0f);
            const bool selected = view.selectedClipId == group.id;
            ImVec4 body = pal.accentDeep;
            if (selected) {
                body = ImVec4(body.x + 0.08f, body.y + 0.08f, body.z + 0.08f,
                              1.0f);
            }
            dl->AddRectFilled(gMin, gMax, C(body), 3.0f);
            dl->AddRect(gMin, gMax,
                        selected ? C(pal.accent) : C(pal.accentStrong), 3.0f);
            if (gw > 28.0) {
                ImFont* groupFont = theme::fonts().small != nullptr
                    ? theme::fonts().small : ImGui::GetFont();
                dl->PushClipRect(ImVec2(gMin.x + 6.0f, gMin.y),
                                 ImVec2(gMax.x - 4.0f, gMax.y), true);
                dl->AddText(groupFont,
                            static_cast<float>(theme::typeScale().caption),
                            ImVec2(gMin.x + 6.0f, gMin.y + 3.0f),
                            C(pal.primaryText), group.name.c_str());
                dl->PopClipRect();
            }

            const bool groupHovered =
                mouse.x >= gMin.x && mouse.x <= gMax.x &&
                mouse.y >= gMin.y && mouse.y <= gMax.y;
            if (!selectorMode && areaHovered && groupHovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // The group is the drag target now, so it uses the clip drag
                // machinery with its own id — one gesture, one code path.
                view.selectedClipId = group.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                selectClipRange(view, transport, static_cast<int>(ti),
                                group.timelineStart, group.length);
                view.dragClipOriginalStart = group.timelineStart;
                view.dragPreviewStart = group.timelineStart;
                view.dragOriginalTrackId = track.id;
                view.dragClipIsMidi = false;
                view.dragKind = TimelineViewState::DragKind::AudioClip;
            }
            if (areaHovered && groupHovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                view.selectedClipId = group.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                ImGui::OpenPopup("##clip_ctx");
            }
        }

        {
            float laneTop = y + trackHeight;
            const auto lanes = lanesOf(track.id);
            for (size_t li = 0; li < lanes.size(); ++li) {
                const LaneMode laneMode = lanes[li];
                if (laneMode == LaneMode::Automation) {
                    drawChannelAutomationLane(track.id, track.volumeAutomation,
                                              track.panAutomation, laneTop);
                } else {
                    drawChainLane(track, laneTop, laneMode,
                                  static_cast<int>(li));
                }
                laneTop += laneHeightFor(track, laneMode);
            }
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


        if (view.recordingActive && track.recordArm) {
            const int64_t liveEnd = std::max(
                view.recordingStartSample, view.recordingEndSample);
            const float x1 = static_cast<float>(
                origin.x + gutterWidth +
                (view.recordingStartSample - view.scrollSamples) /
                    view.samplesPerPixel);
            const float x2 = static_cast<float>(
                origin.x + gutterWidth +
                (liveEnd - view.scrollSamples) / view.samplesPerPixel);
            const float laneLeft = origin.x + gutterWidth;
            const float laneRight = origin.x + totalWidth;
            if (x2 >= laneLeft && x1 <= laneRight) {
                const ImVec2 previewMin(std::max(x1, laneLeft), y + 6.0f);
                const ImVec2 previewMax(
                    std::min(std::max(x2, x1 + 2.0f), laneRight),
                    y + trackHeight - 6.0f);
                dl->AddRectFilled(previewMin, previewMax,
                                  IM_COL32(196, 48, 54, 70), 2.0f);
                dl->AddRect(previewMin, previewMax,
                            IM_COL32(230, 73, 79, 220), 2.0f, 0, 1.5f);
            }
        }

        // Click the gutter to select this track.
        if (gutterHovered &&
            mouse.y >= y && mouse.y <= y + trackHeight &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.selectedTrackIndex = static_cast<int>(ti);
        }

        for (const auto& clip : track.clips) {
            // A grouped clip does not draw itself: the group's own clip stands
            // in for it, over the range the group was made from. The audio is
            // still there and still plays — this changes what you can see and
            // grab, not what exists.
            if (edit.clipGroupContaining(track.id, clip.id) != nullptr) {
                continue;
            }
            // A drag previews from view state instead of writing to the clip.
            // Mutating the document during the drag meant MoveClipCommand
            // snapshotted "the old position" after it had already been
            // overwritten, so undo restored the dragged-to position and the
            // move became permanent.
            const bool clipIsDragging =
                view.isDragging(TimelineViewState::DragKind::AudioClip) &&
                view.selectedClipId == clip.id;
            // A clip being moved is drawn once, on top, at the pointer's row
            // after every track (below) — so it follows the cursor freely
            // between tracks, leaves nothing behind on its own row, and needs
            // no separate ghost. A trim stays in place and is drawn here.
            if (clipIsDragging && !ImGui::GetIO().KeyAlt) continue;
            const bool clipIsTrimming = view.isTrimming() &&
                !view.dragClipIsMidi && view.selectedClipId == clip.id;
            const int64_t drawStart = (clipIsDragging || clipIsTrimming)
                ? view.dragPreviewStart : clip.timelineStart;
            const int64_t drawLength =
                clipIsTrimming ? view.dragPreviewLength : clip.length;
            const int64_t drawOffset =
                clipIsTrimming ? view.dragPreviewOffset : clip.sourceOffset;
            double clipX = origin.x + gutterWidth +
                (drawStart - view.scrollSamples) / view.samplesPerPixel;
            double clipW = static_cast<double>(drawLength) / view.samplesPerPixel;
            if (clipW < 2) clipW = 2;
            if (clipX + clipW < origin.x + gutterWidth) continue;
            if (clipX > origin.x + totalWidth) continue;

            struct Rect { ImVec2 min, max; bool contains(const ImVec2& p) const {
                return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; } };
            Rect clipRect{ImVec2(clipX, y + 6),
                          ImVec2(clipX + clipW, y + trackHeight - 6)};
            // A grouped clip does not draw itself: the group's own clip
            // stands in for it, over the range the group was made from. The
            // audio is still there and still plays — this is what you can see
            // and grab, not what exists.
            if (edit.clipGroupContaining(track.id, clip.id) != nullptr) {
                continue;
            }
            // Confine the clip's drawing to the lane: a clip scrolled so its
            // start sits under the gutter must not paint over the track header
            // and its controls.
            dl->PushClipRect(ImVec2(origin.x + gutterWidth, y),
                             ImVec2(origin.x + totalWidth, y + trackHeight), true);
            bool isSel = view.selectedClipId == clip.id ||
                        view.selectedClipIds.count(clip.id) > 0;
            // A distinct header gives a clip its identity without competing
            // with the waveform, which carries the edit-critical detail.
            ImVec4 body = pal.clipAudio;
            if (isSel) body = ImVec4(body.x + 0.1f, body.y + 0.08f, body.z + 0.05f, 1.0f);
            // A muted clip reads as switched off: desaturated and dimmed, so a
            // glance tells it apart from one that will play.
            if (clip.muted) {
                const float g = (body.x + body.y + body.z) / 3.0f;
                body = ImVec4(g * 0.6f + body.x * 0.2f, g * 0.6f + body.y * 0.2f,
                              g * 0.6f + body.z * 0.2f, 0.55f);
            }
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
            if (bufIt != assetBuffers.end() && bufIt->second) {
                const int spp = std::max(1, static_cast<int>(view.samplesPerPixel));
                const auto& level = peaks.get(
                    clip.asset.sha256, bufIt->second->channels, spp);
                const float waveformTop = headerBottom + 4.0f;
                const float waveformBottom = clipRect.max.y - 4.0f;
                const float waveformMidY = (waveformTop + waveformBottom) * 0.5f;
                const float waveformHalfH = std::max(2.0f,
                    (waveformBottom - waveformTop) * 0.5f);
                const float waveformScale = level.displayScale;
                int64_t numDraw = static_cast<int64_t>(clipW);
                ImU32 waveCol = C(pal.clipAudioBorder);
                for (int64_t px = 0; px < numDraw; ++px) {
                    const int64_t sourceStart = drawOffset +
                        static_cast<int64_t>(std::floor(px * view.samplesPerPixel));
                    const int64_t sourceEnd = drawOffset +
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

            // Fades: paint the in/out ramps over the clip so the picture
            // matches what AudioClipNode plays — same shape function, so the
            // curve you see is the gain you hear. The darkened wedge is the
            // attenuated part; the bright line is the envelope.
            {
                const float top = clipRect.min.y;
                const float bottom = clipRect.max.y;
                const float height = bottom - top;
                const ImU32 fadeLine = C(pal.accent);
                const ImU32 fadeFill = IM_COL32(0, 0, 0, 90);
                constexpr int kFadeSegments = 24;
                const float clipRight = static_cast<float>(clipX + clipW);
                auto drawFade = [&](document::FadeShape shape, int64_t lenSamples,
                                    bool fadeOut) {
                    if (lenSamples <= 0) return;
                    float widthPx = static_cast<float>(
                        static_cast<double>(lenSamples) / view.samplesPerPixel);
                    if (widthPx < 1.5f) return;  // sub-pixel: not worth drawing
                    widthPx = std::min(widthPx, static_cast<float>(clipW));
                    const float edgeX = fadeOut ? clipRight
                                                : static_cast<float>(clipX);
                    ImVec2 prev;
                    for (int s = 0; s <= kFadeSegments; ++s) {
                        const float t = static_cast<float>(s) / kFadeSegments;
                        const float g = fadeOut ? document::fadeOutGain(shape, t)
                                                : document::fadeInGain(shape, t);
                        const float x = fadeOut ? edgeX - widthPx + widthPx * t
                                                : edgeX + widthPx * t;
                        const ImVec2 cur(x, bottom - g * height);
                        if (s > 0) {
                            // Trapezoid strip filling the attenuated area above
                            // the curve — always renders right, unlike a single
                            // concave polygon fill.
                            dl->AddQuadFilled(ImVec2(prev.x, top),
                                              ImVec2(cur.x, top), cur, prev,
                                              fadeFill);
                            dl->AddLine(prev, cur, fadeLine, 1.5f);
                        }
                        prev = cur;
                    }
                };
                drawFade(clip.fadeInShape, clip.fadeIn, false);
                drawFade(clip.fadeOutShape, clip.fadeOut, true);
            }

            dl->PopClipRect();

            // Interaction: drag to move.
                const bool clipHovered = clipRect.contains(mouse);
            const auto audioGesture =
                clipGestureAt(mouse.x, clipRect.min.x, clipRect.max.x);
            if (areaHovered && clipHovered &&
                audioGesture != TimelineViewState::DragKind::None) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (areaHovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) && clipHovered) {
                const bool shift = ImGui::GetIO().KeyShift;
                // Command is the selector: it stands down here so the range
                // machinery marks a span in this lane instead of grabbing the
                // clip. Shift builds a multi-clip selection; a plain click
                // selects the one clip and arms move/trim.
                if (!selectorMode) {
                    applyClipSelection(view, edit, static_cast<int>(ti),
                                       clip.id, shift, false);
                    if (!shift) {
                        selectClipRange(view, transport, static_cast<int>(ti),
                                        clip.timelineStart, clip.length);
                        view.dragClipOriginalStart = clip.timelineStart;
                        view.dragPreviewStart = clip.timelineStart;
                        view.dragClipOriginalOffset = clip.sourceOffset;
                        view.dragPreviewOffset = clip.sourceOffset;
                        view.dragClipOriginalLength = clip.length;
                        view.dragPreviewLength = clip.length;
                        view.dragOriginalTrackId = track.id;
                        view.dragClipIsMidi = false;
                        view.dragKind =
                            audioGesture == TimelineViewState::DragKind::None
                                ? TimelineViewState::DragKind::AudioClip
                                : audioGesture;
                    }
                }
            }
            // Right-click: context menu (split, delete).
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                ImGui::OpenPopup("##clip_ctx");
            }
        }

        if (selected && view.showTransientTicks) {
            std::vector<TransientTickPosition> ticks;
            const float threshold = audio::TransientDetector::
                thresholdForSensitivity(view.transientSensitivity);
            for (const auto& clip : track.clips) {
                const auto analysis = transientAnalyses.find(clip.asset.sha256);
                if (analysis == transientAnalyses.end() ||
                    analysis->second.status !=
                        audio::TransientAnalysisCache::Status::Ready ||
                    !analysis->second.candidates || clip.length <= 0) {
                    continue;
                }
                const int64_t sourceBegin = std::max<int64_t>(0, clip.sourceOffset);
                const int64_t sourceEnd = sourceBegin <=
                        std::numeric_limits<int64_t>::max() - clip.length
                    ? sourceBegin + clip.length
                    : std::numeric_limits<int64_t>::max();
                for (const auto& candidate : *analysis->second.candidates) {
                    if (candidate.strength < threshold ||
                        candidate.sourceSample < sourceBegin ||
                        candidate.sourceSample >= sourceEnd) {
                        continue;
                    }
                    const int64_t candidateOffset =
                        candidate.sourceSample - sourceBegin;
                    if (clip.timelineStart >
                        std::numeric_limits<int64_t>::max() - candidateOffset) {
                        continue;
                    }
                    const int64_t timelineSample =
                        clip.timelineStart + candidateOffset;
                    const float x = static_cast<float>(origin.x + gutterWidth +
                        (timelineSample - view.scrollSamples) /
                            view.samplesPerPixel);
                    if (x >= origin.x + gutterWidth && x <= origin.x + totalWidth) {
                        ticks.push_back({x, candidate.strength});
                    }
                }
            }
            const auto culled = cullTransientTicks(std::move(ticks));
            const ImU32 tickColor = IM_COL32(
                static_cast<int>(pal.accent.x * 255.0f),
                static_cast<int>(pal.accent.y * 255.0f),
                static_cast<int>(pal.accent.z * 255.0f), 190);
            for (const auto& tick : culled) {
                dl->AddLine(ImVec2(tick.x, y + 5.0f),
                            ImVec2(tick.x, y + trackHeight - 5.0f),
                            tickColor, 1.0f);
            }
        }
    }

    ImGui::PopStyleVar();

    // Picture is an opt-in: a session that never imported video has no lane,
    // so the timeline shows only what the project actually contains.
    const bool hasVideo = !edit.videoTracks().empty();
    const float videoLaneY = tracksTop + tracksRegionHeight;
    const float videoLaneHeight = hasVideo ? 40.0f : 0.0f;
    const bool canvasHovered =
        windowHeldOrHovered &&
        mouse.x >= origin.x + gutterWidth &&
        mouse.x <= origin.x + totalWidth &&
        mouse.y >= tracksTop &&
        mouse.y <= origin.y + totalHeight &&
        !(hasVideo && mouse.y >= videoLaneY &&
          mouse.y <= videoLaneY + videoLaneHeight);

    // Holding the command modifier switches to the range selector; show the
    // I-beam over the timeline so the mode is visible before the drag begins.
    // Set after the clip loops so it wins over their resize cursor — selector
    // mode suppresses trims, so the pointer should read as "select", not
    // "resize", even over a clip edge.
    if (selectorMode && (canvasHovered || rulerHovered)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

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
                if (ImGui::MenuItem("Duplicate", "Cmd+D")) {
                    undo.execute(std::make_unique<editing::DuplicateClipCommand>(
                        trk.id, view.selectedClipId));
                }
                // Per-clip gain and mute. The gain is seeded from the clip when
                // the menu opens and committed once the slider is released, so
                // dragging it is a single undo step rather than one per frame.
                const document::AudioClip* ac = nullptr;
                for (const auto& c : trk.clips) {
                    if (c.id == view.selectedClipId) { ac = &c; break; }
                }
                if (ac != nullptr) {
                    ImGui::Separator();
                    if (ImGui::IsWindowAppearing()) {
                        view.contextClipGainDb = ac->gain > 0.0
                            ? static_cast<float>(20.0 * std::log10(ac->gain))
                            : -60.0f;
                    }
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::SliderFloat("Gain", &view.contextClipGainDb, -60.0f,
                                       12.0f, "%.1f dB");
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        const double lin = view.contextClipGainDb <= -59.9f
                            ? 0.0
                            : std::pow(10.0, view.contextClipGainDb / 20.0);
                        undo.execute(
                            std::make_unique<editing::SetClipGainCommand>(
                                trk.id, view.selectedClipId, lin));
                    }
                    if (ImGui::MenuItem(ac->muted ? "Unmute" : "Mute", "Cmd+M")) {
                        undo.execute(
                            std::make_unique<editing::SetClipMuteCommand>(
                                trk.id, view.selectedClipId, !ac->muted));
                    }
                }
                ImGui::Separator();
                const bool isGroup =
                    edit.clipGroup(view.selectedClipId) != nullptr;
                if (ImGui::MenuItem("Group", "Cmd+Opt+G", false, !isGroup)) {
                    groupSelectedClips(const_cast<document::Edit&>(edit), undo,
                                       view);
                }
                if (ImGui::MenuItem("Ungroup", "Cmd+Opt+U", false, isGroup)) {
                    ungroupSelectedClips(const_cast<document::Edit&>(edit),
                                         undo, view);
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

    // MIDI clip context menu. The owning track comes straight from the
    // selection now: selectedTrackIndex indexes the one track list, so it no
    // longer needs a separate id to survive a lossy band decode.
    if (ImGui::BeginPopup("##midi_clip_ctx")) {
        const int row = view.selectedTrackIndex;
        if (!view.selectedClipId.empty() && row >= 0 &&
            row < static_cast<int>(tracks.size())) {
            const std::string& trackId = tracks[static_cast<size_t>(row)].id;
            if (ImGui::MenuItem("Split at Playhead")) {
                undo.execute(std::make_unique<editing::SplitMidiClipCommand>(
                    trackId, view.selectedClipId, transport.position()));
            }
            if (ImGui::MenuItem("Duplicate", "Cmd+D")) {
                undo.execute(std::make_unique<editing::DuplicateMidiClipCommand>(
                    trackId, view.selectedClipId));
            }
            ImGui::Separator();
            const bool grouped =
                edit.clipGroupContaining(trackId, view.selectedClipId) !=
                nullptr;
            if (ImGui::MenuItem("Group", "Cmd+Opt+G", false, !grouped)) {
                groupSelectedClips(const_cast<document::Edit&>(edit), undo,
                                   view);
            }
            if (ImGui::MenuItem("Ungroup", "Cmd+Opt+U", false, grouped)) {
                ungroupSelectedClips(const_cast<document::Edit&>(edit), undo,
                                     view);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Clip")) {
                undo.execute(std::make_unique<editing::RemoveMidiClipCommand>(
                    trackId, view.selectedClipId));
                view.selectedClipId.clear();
            }
        }
        ImGui::EndPopup();
    }

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
        // Which row is the mouse over?
        // One list, so the row under the mouse is the row under the mouse
        // whichever kind of clip is in flight.
        const int targetTrackIndex = indexAtY(mouse.y, tracksTop, audioOffsets);

        // Preview only — the document is not touched until mouse-up, so
        // MoveClipCommand can snapshot a genuine "before" for undo, and the
        // engine never sees a position the user hasn't committed to.
        const int64_t dxPx =
            static_cast<int64_t>(mouse.x - view.dragStartMouseX);
        int64_t dxSamples = static_cast<int64_t>(dxPx * view.samplesPerPixel);
        {
            // A press that hasn't moved leaves the clip exactly where it is:
            // snapping it to the grid on a mere click would nudge an off-grid
            // clip, and shift the selection that rides with it off the clip's
            // own edges — which is what stopped "click a clip, press F" from
            // reading as the whole clip.
            if (dxPx == 0) {
                view.dragPreviewStart = view.dragClipOriginalStart;
            } else {
                int64_t raw = std::max<int64_t>(
                    0, view.dragClipOriginalStart + dxSamples);
                view.dragPreviewStart = snapPosition(raw);
            }
        }
        // Whether the thing in flight is a group is asked once and reused for
        // both the selection follow and the draw below.
        bool draggingGroup = false;
        for (const auto& g : edit.clipGroups()) {
            if (g.id == view.selectedClipId) { draggingGroup = true; break; }
        }
        // The selection travels with the clip. Clicking a clip selected its
        // range, so the highlight begins on the clip; as the clip moves, the
        // range moves the same amount and onto the same track — otherwise it is
        // left behind pointing at empty timeline.
        if (view.hasSelection) {
            const int64_t selLen = view.selectionEnd - view.selectionStart;
            view.selectionStart = view.dragPreviewStart;
            view.selectionEnd = view.dragPreviewStart + selLen;
            view.selectionAnchor = view.selectionStart;
            view.selectionFocus = view.selectionEnd;
            // A group slides horizontally and keeps its row; a lone clip follows
            // the pointer onto whatever track it lands on.
            if (!draggingGroup && targetTrackIndex >= 0) {
                view.selectionRow = targetTrackIndex;
                view.selectionRowEnd = targetTrackIndex;
            }
        }
        // The dragged clip itself, drawn once here — after every track, so it
        // stays on top whichever way it moves — on the row under the pointer.
        // It IS the clip, not a ghost beside a copy: nothing is left on the
        // original row, and it follows the cursor onto any track.
        //
        // A group is exempt: it drags horizontally on its own row (its members
        // keep their tracks) and is drawn by the group loop above, so relocating
        // it here would paint a second, wrong-sized clip.
        if (!draggingGroup) {
            // Land it on the row under the pointer; off the lanes (e.g. below
            // the last track) keep it on its own row so it stays visible until
            // dropped, where it will make a new track.
            float rowTop;
            float rowHeight;
            if (targetTrackIndex >= 0) {
                rowTop = tracksTop +
                         audioOffsets[static_cast<size_t>(targetTrackIndex)];
                rowHeight =
                    baseHeightOf(tracks[static_cast<size_t>(targetTrackIndex)]);
            } else {
                size_t oi = 0;
                for (; oi < tracks.size(); ++oi) {
                    if (tracks[oi].id == view.dragOriginalTrackId) break;
                }
                rowTop = oi < tracks.size()
                             ? tracksTop + audioOffsets[oi] : tracksTop;
                rowHeight =
                    oi < tracks.size() ? baseHeightOf(tracks[oi]) : trackHeight;
            }
            const float cx = origin.x + gutterWidth +
                static_cast<float>((view.dragPreviewStart - view.scrollSamples) /
                                   view.samplesPerPixel);
            const float cw = std::max(2.0f,
                static_cast<float>(view.dragClipOriginalLength /
                                   view.samplesPerPixel));
            const ImVec4 base = draggingMidiClip ? pal.clipMidi : pal.clipAudio;
            const ImVec4 border = draggingMidiClip ? pal.clipMidiBorder
                                                   : pal.clipAudioBorder;
            dl->PushClipRect(ImVec2(origin.x + gutterWidth, rowTop),
                             ImVec2(origin.x + totalWidth, rowTop + rowHeight),
                             true);
            const ImVec2 cmin(cx, rowTop + 6.0f);
            const ImVec2 cmax(cx + cw, rowTop + rowHeight - 6.0f);
            dl->AddRectFilled(cmin, cmax, C(base), 2.0f);
            // The accent border marks it as the live, selected clip.
            dl->AddRect(cmin, cmax, C(pal.accent), 2.0f);
            const float headerBottom = std::min(cmax.y - 4.0f, cmin.y + 20.0f);
            dl->AddRectFilled(cmin, ImVec2(cmax.x, headerBottom),
                              C(ImVec4(border.x, border.y, border.z, 0.40f)),
                              2.0f, ImDrawFlags_RoundCornersTop);
            dl->PopClipRect();
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Commit via command. If the mouse ended on a different track, move it.
            std::string targetTrackId = view.dragOriginalTrackId;
            if (targetTrackIndex >= 0) {
                targetTrackId = tracks[targetTrackIndex].id;
            }
            const std::string newTrackArg =
                (targetTrackId != view.dragOriginalTrackId) ? targetTrackId : "";
            // Skip a no-op drag so a stray click doesn't push an undo entry
            // that appears to do nothing when replayed.
            const bool moved = view.dragPreviewStart != view.dragClipOriginalStart ||
                               !newTrackArg.empty();
            // A clip in a group drags the whole group, by the same amount —
            // along the timeline and across tracks, every member shifting the
            // same number of rows so the group keeps its shape. The document
            // refuses a row shift that would push a member off the list.
            // The dragged id is a group's when a group clip was grabbed —
            // there is nothing else on the timeline it could be.
            const auto* group = edit.clipGroup(view.selectedClipId);
            // Option held at the drop leaves the original and drops a copy —
            // the standard "duplicate by dragging". Groups keep moving (copying
            // a whole group is a niche the drag machinery doesn't cover yet).
            const bool altCopy = ImGui::GetIO().KeyAlt && group == nullptr;
            if (moved && group != nullptr) {
                int rowDelta = 0;
                if (targetTrackIndex >= 0) {
                    for (size_t oi = 0; oi < tracks.size(); ++oi) {
                        if (tracks[oi].id == view.dragOriginalTrackId) {
                            rowDelta = targetTrackIndex - static_cast<int>(oi);
                            break;
                        }
                    }
                }
                undo.execute(std::make_unique<editing::MoveClipGroupCommand>(
                    group->id,
                    view.dragPreviewStart - view.dragClipOriginalStart,
                    rowDelta));
            } else if (moved && altCopy) {
                if (draggingMidiClip) {
                    undo.execute(std::make_unique<editing::CopyMidiClipToCommand>(
                        view.dragOriginalTrackId, view.selectedClipId,
                        targetTrackId, view.dragPreviewStart));
                } else {
                    undo.execute(std::make_unique<editing::CopyClipToCommand>(
                        view.dragOriginalTrackId, view.selectedClipId,
                        targetTrackId, view.dragPreviewStart));
                }
            } else if (moved) {
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
            // The clip stays selected after a click, move or trim — that is what
            // Cmd+D, Cmd+M and the clip inspector all act on. Only the drag
            // bookkeeping is reset here.
            view.dragKind = TimelineViewState::DragKind::None;
            view.dragOriginalTrackId.clear();
        }
    }

    // Trim drag: preview the whole (start, sourceOffset, length) triple and
    // commit one command on release, for the same reason the move drag does —
    // the command has to be able to snapshot a genuine "before".
    if (view.isTrimming() && !view.selectedClipId.empty()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.dragStartMouseX = mouse.x;
            view.dragStartMouseY = mouse.y;
        }
        const int64_t origStart = view.dragClipOriginalStart;
        const int64_t origOffset = view.dragClipOriginalOffset;
        const int64_t origLength = view.dragClipOriginalLength;
        const int64_t origEnd = origStart + origLength;
        // A clip has to keep some width, or it becomes something the user can
        // neither see nor grab back.
        constexpr int64_t kMinLength = 64;

        // Move the edge BY the mouse's travel, not TO the mouse. Grabbing a
        // handle six pixels inside the edge must not snap the edge to the
        // cursor the instant the button goes down.
        const int64_t dxSamples = static_cast<int64_t>(
            (mouse.x - view.dragStartMouseX) * view.samplesPerPixel);

        // How much source lies either side of the current window. Trimming
        // past it asks for audio the file doesn't contain, so the box stops
        // where the source does. An unknown asset (no length recorded, or a
        // MIDI clip) means no ceiling — only the floor at offset zero applies.
        int64_t headroom = origOffset;
        int64_t tailroom = std::numeric_limits<int64_t>::max();
        if (!view.dragClipIsMidi) {
            if (const auto* trk = edit.track(view.dragOriginalTrackId)) {
                for (const auto& c : trk->clips) {
                    if (c.id != view.selectedClipId) continue;
                    if (const auto* a = edit.asset(c.asset)) {
                        if (a->lengthSamples > 0) {
                            tailroom = std::max<int64_t>(
                                0, a->lengthSamples - (origOffset + origLength));
                        }
                    }
                    break;
                }
            }
        }

        if (view.dragKind == TimelineViewState::DragKind::TrimStart) {
            // The head moves start and offset together: the box shrinks from
            // the left and the audio inside it stays put on the timeline.
            const int64_t newStart = std::clamp(
                snapPosition(std::max<int64_t>(0, origStart + dxSamples)),
                std::max<int64_t>(0, origStart - headroom),
                origEnd - kMinLength);
            const int64_t delta = newStart - origStart;
            view.dragPreviewStart = newStart;
            view.dragPreviewOffset = origOffset + delta;
            view.dragPreviewLength = origLength - delta;
        } else {
            const int64_t newEnd = std::clamp(
                snapPosition(std::max<int64_t>(0, origEnd + dxSamples)),
                origStart + kMinLength,
                tailroom == std::numeric_limits<int64_t>::max()
                    ? std::numeric_limits<int64_t>::max()
                    : origEnd + tailroom);
            view.dragPreviewStart = origStart;
            view.dragPreviewOffset = origOffset;
            view.dragPreviewLength = newEnd - origStart;
        }

        // The selection reshapes with the clip, the same way it follows a move:
        // clicking the clip selected its range, so trimming an edge should drag
        // that edge of the selection too rather than leave it on the old one.
        if (view.hasSelection) {
            view.selectionStart = view.dragPreviewStart;
            view.selectionEnd = view.dragPreviewStart + view.dragPreviewLength;
            view.selectionAnchor = view.selectionStart;
            view.selectionFocus = view.selectionEnd;
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // A press that never moved must not push an undo entry that
            // replays as a no-op.
            const bool trimmed = view.dragPreviewStart != origStart ||
                                 view.dragPreviewLength != origLength;
            if (trimmed) {
                if (view.dragClipIsMidi) {
                    undo.execute(std::make_unique<editing::TrimMidiClipCommand>(
                        view.dragOriginalTrackId, view.selectedClipId,
                        view.dragPreviewStart, view.dragPreviewOffset,
                        view.dragPreviewLength));
                } else {
                    undo.execute(std::make_unique<editing::TrimClipCommand>(
                        view.dragOriginalTrackId, view.selectedClipId,
                        view.dragPreviewStart, view.dragPreviewOffset,
                        view.dragPreviewLength));
                }
            }
            view.dragKind = TimelineViewState::DragKind::None;
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

    // Selection edges use the same optional format snap as every other
    // timeline edit rather than silently snapping while the toggle is off.
    auto selectionSampleAtMouseX = [&]() -> int64_t {
        return snapPosition(sampleAtMouseX());
    };

    // Ruler: the press seeks, and dragging from it selects the range across
    // every track — the one gesture that reaches all lanes at once. (This
    // replaces drag-to-scrub: the playhead can follow the mouse or the drag
    // can mark a range, not both.)
    if (rulerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // While stopped the press seeks; while playing it only begins a range,
        // so dragging out a selection never interrupts playback. A plain click
        // (no drag) still relocates on release, via the click path below. The
        // selector (command) never seeks — it is for marking, not locating.
        // Shift: the range runs from the playhead to the click, the way a
        // shift-click extends a selection from the cursor in any editor.
        const bool fromPlayhead = ImGui::GetIO().KeyShift;
        if (!transport.isPlaying() && !selectorMode && !fromPlayhead) {
            seekToMouseX();
        }
        view.selectionPressSample = selectionSampleAtMouseX();
        const int64_t s = selectionSampleAtMouseX();
        const int64_t anchor = fromPlayhead ? transport.position() : s;
        view.selectionStart = anchor;
        view.selectionEnd = s;
        view.selectionAnchor = anchor;
        view.selectionFocus = s;
        view.selectionRow = -1;          // -1 = all tracks
        view.selectionRowEnd = -1;
        view.isSelecting = true;
        view.hasSelection = true;
        view.selectionFromPlayhead = fromPlayhead;
    }

    // Track area: click empty space seeks (but not on clips — those drag).
    if (canvasHovered && !automationMouseOver && !automationConsumedClick &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !view.isDragging() && !view.draggingAutomation) {
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
            for (const auto& clip : track.midiClips) {
                if (spansMouse(clip.timelineStart, clip.length)) { hitClip = true; break; }
            }
            if (hitClip) break;
        }
        if (!hitClip || selectorMode) {
            // Empty lane, or the Command selector held over a clip: start a
            // range selection. Command is the selector tool, so a press on a
            // clip with Command marks a time range within the clip's lane
            // rather than grabbing the clip (the clip handler stands down for
            // selectorMode). A plain press on a clip still goes to the clip
            // handler and drags. Dragging a fresh range drops any multi-clip
            // selection.
            view.selectedClipIds.clear();
            const int row = rowAtY(mouse.y);
            view.selectionPressSample = selectionSampleAtMouseX();
            const int64_t s = selectionSampleAtMouseX();
            // Shift anchors the range at the playhead instead of the click.
            const bool fromPlayhead = ImGui::GetIO().KeyShift;
            const int64_t anchor = fromPlayhead ? transport.position() : s;
            view.selectionStart = anchor;
            view.selectionEnd = s;
            view.selectionAnchor = anchor;
            view.selectionFocus = s;
            view.selectionRow = row;
            view.selectionRowEnd = row;
            view.isSelecting = true;
            view.selectionFromPlayhead = fromPlayhead;
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
        view.selectionFocus = view.selectionEnd;
        // Dragging onto other rows widens the selection across them; a
        // lane selection is a rectangle of tracks, not a single row.
        if (view.selectionRow >= 0) {
            const int row = rowAtY(mouse.y);
            if (row >= 0) view.selectionRowEnd = row;
        }
    }
    // End selection on mouse release.
    if (view.isSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        view.isSelecting = false;
        // If the selection is tiny (just a click, not a drag), treat as seek +
        // clear selection — unless the selector (command) is held, whose whole
        // point is to select without disturbing the playhead.
        const bool fromPlayhead = view.selectionFromPlayhead;
        view.selectionFromPlayhead = false;
        if (std::abs(view.selectionEnd - view.selectionStart) <
            static_cast<int64_t>(4 * view.samplesPerPixel)) {
            // Not a drag after all: the cursor uses the same snap mode as the
            // press, so Snap never changes meaning between click and drag.
            if (!selectorMode && !fromPlayhead) {
                transport.seek(view.selectionPressSample);
            }
            view.hasSelection = false;
        } else if (fromPlayhead) {
            // Anchored at the playhead on purpose: leave it there.
        } else if (view.hasSelection) {
            // A finished selection puts the cursor at its head, so play
            // starts from the top of the range — and, with loop on, so does
            // the loop. The head is the lower edge whichever direction the
            // drag ran: dragging right to left selects the same range, and
            // dropping the cursor at the pointer would leave it at the tail.
            // Not while playing, and not under the selector: both cases want
            // the range marked without the playhead snapping to it.
            if (!transport.isPlaying() && !selectorMode) {
                transport.seek(std::min(view.selectionStart, view.selectionEnd));
            }
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
        const int rowEnd =
            view.selectionRowEnd >= 0 ? view.selectionRowEnd : view.selectionRow;
        const int lo = std::min(view.selectionRow, rowEnd);
        const int hi = std::max(view.selectionRow, rowEnd);
        float loY = 0.0f, loH = 0.0f, hiY = 0.0f, hiH = 0.0f;
        if (rowExtent(lo, loY, loH) && rowExtent(hi, hiY, hiH)) {
            selTop = loY;
            selBottom = hiY + hiH;
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

    dl->PopClipRect();  // end the scrolling track-rows region

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
    // Resolve any audio-file drops against the current track/sample geometry.
    if (!view.pendingFileDrops.empty()) {
        const float laneLeft = origin.x + gutterWidth;
        for (const auto& drop : view.pendingFileDrops) {
            TimelineViewState::ResolvedFileDrop resolved;
            resolved.path = drop.path;
            const int row = indexAtY(drop.y, tracksTop, audioOffsets);
            if (row >= 0 && row < static_cast<int>(tracks.size()) &&
                drop.x >= laneLeft) {
                resolved.trackId = tracks[static_cast<size_t>(row)].id;
                const int64_t raw = std::max<int64_t>(0,
                    static_cast<int64_t>(view.scrollSamples +
                        (drop.x - laneLeft) * view.samplesPerPixel));
                resolved.sample = snapPosition(raw);
            }
            view.resolvedFileDrops.push_back(std::move(resolved));
        }
        view.pendingFileDrops.clear();
    }

    // Live drop preview: a ghost clip under the cursor while a file is dragged
    // over, so its landing spot is visible before release.
    if (view.fileDragActive) {
        const float laneLeft = origin.x + gutterWidth;
        const int row = indexAtY(view.fileDragY, tracksTop, audioOffsets);
        if (row >= 0 && row < static_cast<int>(tracks.size()) &&
            view.fileDragX >= laneLeft) {
            const float rowHeight =
                baseHeightOf(tracks[static_cast<size_t>(row)]);
            const float ty = tracksTop + audioOffsets[static_cast<size_t>(row)];
            const int64_t startSample = snapPosition(std::max<int64_t>(0,
                static_cast<int64_t>(view.scrollSamples +
                    (view.fileDragX - laneLeft) * view.samplesPerPixel)));
            const float gx = laneLeft +
                static_cast<float>((startSample - view.scrollSamples) /
                                   view.samplesPerPixel);
            // Known length draws the real region; unknown draws a slim insert.
            const float gw = view.fileDragLengthSamples > 0
                ? std::max(2.0f, static_cast<float>(
                      view.fileDragLengthSamples / view.samplesPerPixel))
                : 3.0f;
            dl->PushClipRect(ImVec2(laneLeft, ty),
                             ImVec2(origin.x + totalWidth, ty + rowHeight), true);
            dl->AddRectFilled(ImVec2(gx, ty + 6.0f),
                              ImVec2(gx + gw, ty + rowHeight - 6.0f),
                              C(ImVec4(pal.clipAudio.x, pal.clipAudio.y,
                                       pal.clipAudio.z, 0.45f)), 2.0f);
            dl->AddRect(ImVec2(gx, ty + 6.0f),
                        ImVec2(gx + gw, ty + rowHeight - 6.0f),
                        C(pal.accentStrong), 2.0f);
            // A bright insertion line at the exact start, so the drop point is
            // unambiguous even for a very short or unknown-length file.
            dl->AddLine(ImVec2(gx, ty + 3.0f), ImVec2(gx, ty + rowHeight - 3.0f),
                        C(pal.accentStrong), 1.5f);
            dl->PopClipRect();
        }
    }

    const double wheel = ImGui::GetIO().MouseWheel;
    const double wheelH = ImGui::GetIO().MouseWheelH;
    const bool timelineHovered = windowHovered || canvasHovered;
    if (timelineHovered && wheel != 0.0) {
        if (ImGui::GetIO().KeyCtrl) {
            // Control: vertical zoom (row height). Taller scrolling up, shorter
            // down; clamped so a row can neither vanish nor swallow the view.
            view.trackHeightScale = std::clamp(
                view.trackHeightScale + static_cast<float>(wheel) * 0.1f,
                0.6f, 3.0f);
        } else if (ImGui::GetIO().KeySuper) {
            // Command: horizontal zoom. (macOS claims Control+scroll for its
            // own screen zoom, so Command is the reachable modifier here.)
            const double zoomStep = std::max(10.0, view.samplesPerPixel * 0.1);
            zoomAroundSample(view, view.samplesPerPixel - wheel * zoomStep,
                             transport.position());
        } else if (ImGui::GetIO().KeyShift) {
            // Shift: scroll the timeline left/right.
            view.scrollSamples = std::max(
                0.0, view.scrollSamples - wheel * view.samplesPerPixel * 20.0);
        } else {
            // Plain wheel: scroll the track rows (not the window, whose scroll
            // would drag the pinned header off the top with them).
            view.verticalScroll = std::clamp(
                view.verticalScroll - static_cast<float>(wheel) * 42.0f, 0.0f,
                maxVerticalScroll);
        }
    }
    // A trackpad's (or a shift-translated) horizontal wheel also scrolls the
    // timeline sideways.
    if (timelineHovered && wheelH != 0.0 &&
        !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper) {
        view.scrollSamples = std::max(
            0.0, view.scrollSamples - wheelH * view.samplesPerPixel * 20.0);
    }

    // Adding reallocates the track vector, so it happens only after every
    // reference used for drawing and interaction has gone out of scope.
    if (addTrackRequested || addTrackMenuRequested) {
        undo.execute(std::make_unique<editing::AddTrackCommand>("Track"));
        view.selectedTrackIndex =
            static_cast<int>(edit.tracks().size()) - 1;
    }
    if (addBusRequested) {
        undo.execute(std::make_unique<editing::AddTrackCommand>("Bus", editing::AddTrackCommand::Flavour::Bus));
        view.selectedTrackIndex = static_cast<int>(
            edit.tracks().size() + edit.tracks().size() +
            edit.tracks().size() - 2);
    }
    if (!view.requestTrackColorId.empty()) {
        undo.execute(std::make_unique<editing::SetTrackColorCommand>(
            view.requestTrackColorId, view.requestTrackColor));
        view.requestTrackColorId.clear();
        view.requestTrackColor.clear();
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

void selectClipRange(TimelineViewState& view, engine::Transport& transport,
                     int row, int64_t start, int64_t length) {
    if (length <= 0) return;
    view.hasSelection = true;
    view.selectionRow = row;
    view.selectionRowEnd = row;
    view.selectionStart = start;
    view.selectionEnd = start + length;
    // Anchor and focus matter for Shift+Tab, which moves whichever edge is
    // not anchored; a clip's head is the natural anchor.
    view.selectionAnchor = view.selectionStart;
    view.selectionFocus = view.selectionEnd;
    // The head of the range, so play starts from the top of what is selected
    // — and, with loop on, so does the loop. Not while already playing, though:
    // selecting mid-playback marks the range without yanking the playhead back.
    if (!transport.isPlaying()) transport.seek(view.selectionStart);
}

std::vector<document::ClipGroup::Member> clipsInRange(
    const document::Edit& edit, int64_t start, int64_t end, int row) {
    std::vector<document::ClipGroup::Member> members;
    if (end <= start) return members;

    const auto& tracks = edit.tracks();
    for (size_t index = 0; index < tracks.size(); ++index) {
        // -1 is a ruler drag, which means every track. Anything else is one
        // lane, and a range dragged in a lane stays in it.
        if (row >= 0 && row != static_cast<int>(index)) continue;
        const auto& track = tracks[index];
        const auto overlaps = [&](int64_t clipStart, int64_t length) {
            return clipStart < end && clipStart + length > start;
        };
        for (const auto& clip : track.clips) {
            if (overlaps(clip.timelineStart, clip.length)) {
                members.push_back({track.id, clip.id, false});
            }
        }
        for (const auto& clip : track.midiClips) {
            if (overlaps(clip.timelineStart, clip.length)) {
                members.push_back({track.id, clip.id, true});
            }
        }
    }
    return members;
}

std::vector<document::ClipGroup::Member> clipsInSelection(
    const document::Edit& edit, const TimelineViewState& view) {
    if (!view.hasSelection) return {};
    return clipsInRange(edit, std::min(view.selectionStart, view.selectionEnd),
                        std::max(view.selectionStart, view.selectionEnd),
                        view.selectionRow);
}

bool groupSelectedClips(document::Edit& edit, editing::UndoStack& undo,
                        TimelineViewState& view) {
    // The selection is what makes the group, so an empty range is fine: it
    // gives an empty group over it, which is a placeholder you can drag and
    // fill later. What is refused is a group with no extent at all.
    int64_t start = std::min(view.selectionStart, view.selectionEnd);
    int64_t end = std::max(view.selectionStart, view.selectionEnd);

    // With no range dragged, the selected clip is the group. Pressing the
    // shortcut with a clip selected and getting nothing is the same silence
    // as pressing it with nothing selected, and one of those is a mistake.
    if (!view.hasSelection || end <= start) {
        if (view.selectedClipId.empty() || view.selectedTrackIndex < 0 ||
            view.selectedTrackIndex >= static_cast<int>(edit.tracks().size())) {
            return false;
        }
        const auto& track =
            edit.tracks()[static_cast<size_t>(view.selectedTrackIndex)];
        const auto extentOf = [&](int64_t clipStart, int64_t length) {
            start = clipStart;
            end = clipStart + length;
        };
        bool found = false;
        for (const auto& clip : track.clips) {
            if (clip.id != view.selectedClipId) continue;
            extentOf(clip.timelineStart, clip.length);
            found = true;
            break;
        }
        if (!found) {
            for (const auto& clip : track.midiClips) {
                if (clip.id != view.selectedClipId) continue;
                extentOf(clip.timelineStart, clip.length);
                found = true;
                break;
            }
        }
        if (!found || end <= start) return false;
    }

    auto members = clipsInRange(edit, start, end,
                                view.hasSelection ? view.selectionRow
                                                  : view.selectedTrackIndex);
    // Clips already in a group stay in it — addClipGroup would drop them
    // anyway, and taking them silently would leave the old group short.
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&](const document::ClipGroup::Member& m) {
                                     return edit.clipGroupContaining(
                                                m.trackId, m.clipId) != nullptr;
                                 }),
                  members.end());
    // The rows the selection covers, so a group over silence still has
    // somewhere to draw. A ruler drag (-1) spans every track.
    std::vector<std::string> trackIds;
    const int scopeRow = view.hasSelection ? view.selectionRow
                                           : view.selectedTrackIndex;
    for (size_t row = 0; row < edit.tracks().size(); ++row) {
        if (scopeRow >= 0 && scopeRow != static_cast<int>(row)) continue;
        trackIds.push_back(edit.tracks()[row].id);
    }
    if (trackIds.empty()) return false;

    // Groups already under the selection become children rather than being
    // ignored, so grouping two groups gives one group containing two — and
    // ungrouping it gives them back.
    std::vector<std::string> childGroupIds;
    for (const auto& group : edit.clipGroups()) {
        if (group.timelineStart >= end || group.end() <= start) continue;
        if (!edit.clipGroupIsTopLevel(group.id)) continue;
        if (scopeRow >= 0) {
            if (scopeRow >= static_cast<int>(edit.tracks().size())) continue;
            const auto& row = edit.tracks()[static_cast<size_t>(scopeRow)];
            if (!group.coversTrack(row.id)) continue;
        }
        childGroupIds.push_back(group.id);
    }

    auto command = std::make_unique<editing::GroupClipsCommand>(
        std::move(members), start, end - start, std::move(trackIds),
        std::move(childGroupIds));
    auto* raw = command.get();
    undo.execute(std::move(command));
    return !raw->groupId().empty();
}

bool ungroupSelectedClips(document::Edit& edit, editing::UndoStack& undo,
                          TimelineViewState& view) {
    std::vector<std::string> groupIds;
    const auto take = [&](const std::string& id) {
        if (id.empty()) return;
        if (std::find(groupIds.begin(), groupIds.end(), id) ==
            groupIds.end()) {
            groupIds.push_back(id);
        }
    };

    // Groups are found by their own range, not through their members. An empty
    // group has no members and would otherwise be impossible to ungroup — the
    // same mistake that made one impossible to see.
    if (view.hasSelection) {
        const int64_t start = std::min(view.selectionStart, view.selectionEnd);
        const int64_t end = std::max(view.selectionStart, view.selectionEnd);
        if (end > start) {
            const auto& tracks = edit.tracks();
            for (const auto& group : edit.clipGroups()) {
                if (group.timelineStart >= end || group.end() <= start) {
                    continue;
                }
                // Only the outermost layer. Taking every group in range would
                // flatten the whole tree in one keystroke instead of peeling
                // one layer off it.
                if (!edit.clipGroupIsTopLevel(group.id)) continue;
                // Scoped to the selection's lane, like every other range edit.
                if (view.selectionRow >= 0) {
                    if (view.selectionRow >=
                        static_cast<int>(tracks.size())) continue;
                    const auto& row =
                        tracks[static_cast<size_t>(view.selectionRow)];
                    if (!group.coversTrack(row.id)) continue;
                }
                take(group.id);
            }
        }
    }

    // A selected group clip is the obvious target, and its id IS the group's.
    take(edit.clipGroup(view.selectedClipId) != nullptr ? view.selectedClipId
                                                        : std::string{});
    // Otherwise the group holding the selected clip.
    if (groupIds.empty() && !view.selectedClipId.empty() &&
        view.selectedTrackIndex >= 0 &&
        view.selectedTrackIndex < static_cast<int>(edit.tracks().size())) {
        const auto& track = edit.tracks()[
            static_cast<size_t>(view.selectedTrackIndex)];
        if (const auto* group =
                edit.clipGroupContaining(track.id, view.selectedClipId)) {
            take(group->id);
        }
    }

    if (groupIds.empty()) return false;
    for (const auto& id : groupIds) {
        undo.execute(std::make_unique<editing::UngroupClipsCommand>(id));
    }
    return true;
}

bool deleteAutomationInSelection(document::Edit& edit,
                                 editing::UndoStack& undo,
                                 TimelineViewState& view) {
    if (!view.hasSelection || view.selectedTrackIndex < 0) return false;
    const auto& tracks = edit.tracks();
    if (view.selectedTrackIndex >= static_cast<int>(tracks.size())) return false;
    const auto& track = tracks[static_cast<size_t>(view.selectedTrackIndex)];
    // Only a lane that is open: deleting points on a lane the user cannot see
    // would be an edit with no visible cause.
    if (!view.expandedTracks.contains(track.id)) return false;

    // A selection dragged right-to-left still names a range.
    const int64_t start = std::min(view.selectionStart, view.selectionEnd);
    const int64_t end = std::max(view.selectionStart, view.selectionEnd);

    auto parameter = AutomationParameter::Volume;
    const auto found = view.automationParameters.find(track.id);
    if (found != view.automationParameters.end()) parameter = found->second;

    if (parameter == AutomationParameter::Pan) {
        const auto* points = edit.panAutomation(track.id);
        if (points == nullptr) return false;
        auto kept = pointsOutsideRange(*points, start, end);
        if (kept.size() == points->size()) return false;
        undo.execute(std::make_unique<editing::ReplacePanAutomationCommand>(
            track.id, std::move(kept)));
        return true;
    }

    const auto* points = edit.volumeAutomation(track.id);
    if (points == nullptr) return false;
    auto kept = pointsOutsideRange(*points, start, end);
    if (kept.size() == points->size()) return false;
    undo.execute(std::make_unique<editing::ReplaceVolumeAutomationCommand>(
        track.id, std::move(kept)));
    return true;
}

bool trimClipsInSelection(document::Edit& edit, editing::UndoStack& undo,
                          TimelineViewState& view) {
    if (!view.hasSelection) return false;
    const int64_t lo = std::min(view.selectionStart, view.selectionEnd);
    const int64_t hi = std::max(view.selectionStart, view.selectionEnd);
    if (hi <= lo) return false;

    const auto& tracks = edit.tracks();

    // The selection's own row when it has one; otherwise every track, so a
    // range dragged across the whole arrangement clears time on all of them.
    std::vector<size_t> rows;
    if (view.selectionRow >= 0 &&
        view.selectionRow < static_cast<int>(tracks.size())) {
        // Every row the selection was dragged across.
        const int rowEnd = view.selectionRowEnd >= 0
            ? std::min(view.selectionRowEnd, static_cast<int>(tracks.size()) - 1)
            : view.selectionRow;
        const int lo = std::min(view.selectionRow, rowEnd);
        const int hi = std::max(view.selectionRow, rowEnd);
        for (int r = lo; r <= hi; ++r) rows.push_back(static_cast<size_t>(r));
    } else {
        for (size_t i = 0; i < tracks.size(); ++i) rows.push_back(i);
    }

    std::vector<std::unique_ptr<editing::Command>> ops;

    // One clip's four cases, shared by audio and MIDI. `makeTrim`/`makeRemove`/
    // `makeAddRight` are the only things that differ between the two, so the
    // decision lives here once and cannot drift between the clip types.
    auto planClip = [&](const std::string& trackId, int64_t start, int64_t length,
                        const auto& makeRemove, const auto& makeTrim,
                        const auto& makeAddRight) {
        const int64_t clipEnd = start + length;
        if (clipEnd <= lo || start >= hi) return;             // no overlap
        if (lo <= start && hi >= clipEnd) {                   // whole clip
            ops.push_back(makeRemove());
        } else if (lo <= start) {                             // head (hi<clipEnd)
            const int64_t cut = hi - start;
            ops.push_back(makeTrim(hi, cut, clipEnd - hi));
        } else if (hi >= clipEnd) {                           // tail (lo>start)
            ops.push_back(makeTrim(start, 0, lo - start));
        } else {                                              // interior split
            ops.push_back(makeTrim(start, 0, lo - start));    // left piece
            ops.push_back(makeAddRight(hi, hi - start, clipEnd - hi));
        }
    };

    for (size_t ti : rows) {
        const auto& track = tracks[ti];
        const std::string trackId = track.id;

        for (const auto& clip : track.clips) {
            const document::AudioClip captured = clip;
            planClip(trackId, clip.timelineStart, clip.length,
                [&] {
                    return std::make_unique<editing::RemoveClipCommand>(
                        trackId, captured.id);
                },
                [&](int64_t newStart, int64_t offsetAdd, int64_t newLength) {
                    return std::make_unique<editing::TrimClipCommand>(
                        trackId, captured.id, newStart,
                        captured.sourceOffset + offsetAdd, newLength);
                },
                [&](int64_t newStart, int64_t offsetAdd, int64_t newLength) {
                    document::AudioClip right = captured;
                    right.id.clear();
                    right.timelineStart = newStart;
                    right.sourceOffset = captured.sourceOffset + offsetAdd;
                    right.length = newLength;
                    return std::make_unique<editing::AddClipCommand>(
                        trackId, std::move(right));
                });
        }

        for (const auto& clip : track.midiClips) {
            const document::MidiClip captured = clip;
            planClip(trackId, clip.timelineStart, clip.length,
                [&] {
                    return std::make_unique<editing::RemoveMidiClipCommand>(
                        trackId, captured.id);
                },
                [&](int64_t newStart, int64_t offsetAdd, int64_t newLength) {
                    return std::make_unique<editing::TrimMidiClipCommand>(
                        trackId, captured.id, newStart,
                        captured.sourceOffset + offsetAdd, newLength);
                },
                [&](int64_t newStart, int64_t offsetAdd, int64_t newLength) {
                    document::MidiClip right = captured;
                    right.id.clear();
                    right.timelineStart = newStart;
                    right.sourceOffset = captured.sourceOffset + offsetAdd;
                    right.length = newLength;
                    return std::make_unique<editing::AddMidiClipCommand>(
                        trackId, std::move(right));
                });
        }
    }

    if (ops.empty()) return false;
    if (ops.size() == 1) {
        undo.execute(std::move(ops.front()));
    } else {
        undo.execute(std::make_unique<editing::CompoundCommand>(
            std::move(ops), "Delete Range"));
    }
    return true;
}

bool duplicateSelectedClip(const document::Edit& edit,
                           editing::UndoStack& undo,
                           TimelineViewState& view) {
    if (view.selectedClipId.empty() || view.selectedTrackIndex < 0) return false;
    const auto& tracks = edit.tracks();
    if (view.selectedTrackIndex >= static_cast<int>(tracks.size())) return false;
    const auto& track = tracks[static_cast<size_t>(view.selectedTrackIndex)];

    // Which vector holds it is a question about the id, not about the row: a
    // track carries audio and MIDI clips at the same time, so asking the row
    // what kind it is would be asking a question it no longer answers.
    for (const auto& clip : track.clips) {
        if (clip.id != view.selectedClipId) continue;
        undo.execute(std::make_unique<editing::DuplicateClipCommand>(
            track.id, view.selectedClipId));
        return true;
    }
    for (const auto& clip : track.midiClips) {
        if (clip.id != view.selectedClipId) continue;
        undo.execute(std::make_unique<editing::DuplicateMidiClipCommand>(
            track.id, view.selectedClipId));
        return true;
    }
    return false;
}

bool toggleSelectedClipMute(const document::Edit& edit,
                            editing::UndoStack& undo, TimelineViewState& view) {
    if (view.selectedClipId.empty() || view.selectedTrackIndex < 0) return false;
    const auto& tracks = edit.tracks();
    if (view.selectedTrackIndex >= static_cast<int>(tracks.size())) return false;
    const auto& track = tracks[static_cast<size_t>(view.selectedTrackIndex)];
    for (const auto& clip : track.clips) {
        if (clip.id != view.selectedClipId) continue;
        undo.execute(std::make_unique<editing::SetClipMuteCommand>(
            track.id, view.selectedClipId, !clip.muted));
        return true;
    }
    return false;
}

namespace {
// The selected audio clip, or nullptr — shared by the keyboard clip edits.
const document::AudioClip* selectedAudioClip(const document::Edit& edit,
                                             const TimelineViewState& view,
                                             std::string& trackIdOut) {
    if (view.selectedClipId.empty() || view.selectedTrackIndex < 0) return nullptr;
    const auto& tracks = edit.tracks();
    if (view.selectedTrackIndex >= static_cast<int>(tracks.size())) return nullptr;
    const auto& track = tracks[static_cast<size_t>(view.selectedTrackIndex)];
    for (const auto& c : track.clips) {
        if (c.id == view.selectedClipId) { trackIdOut = track.id; return &c; }
    }
    return nullptr;
}
}  // namespace

bool trimSelectedClipToSelection(const document::Edit& edit,
                                 editing::UndoStack& undo, TimelineViewState& view,
                                 bool keepAfter) {
    if (!view.hasSelection) return false;
    std::string trackId;
    const document::AudioClip* clip = selectedAudioClip(edit, view, trackId);
    if (clip == nullptr) return false;
    const int64_t clipStart = clip->timelineStart;
    const int64_t clipEnd = clipStart + clip->length;
    // keepAfter (A): cut everything before the selection's start; keep from
    // there on. Otherwise (S): cut everything after the selection's end.
    const int64_t cut = keepAfter
        ? std::min(view.selectionStart, view.selectionEnd)
        : std::max(view.selectionStart, view.selectionEnd);
    if (cut <= clipStart || cut >= clipEnd) return false;  // nothing to cut
    if (keepAfter) {
        const int64_t delta = cut - clipStart;
        undo.execute(std::make_unique<editing::TrimClipCommand>(
            trackId, clip->id, cut, clip->sourceOffset + delta,
            clip->length - delta));
    } else {
        undo.execute(std::make_unique<editing::TrimClipCommand>(
            trackId, clip->id, clipStart, clip->sourceOffset,
            cut - clipStart));
    }
    return true;
}

bool fadeSelectedClipToPlayhead(const document::Edit& edit,
                                editing::UndoStack& undo, TimelineViewState& view,
                                int64_t playhead, bool fadeIn,
                                document::FadeShape shape) {
    std::string trackId;
    const document::AudioClip* clip = selectedAudioClip(edit, view, trackId);
    if (clip == nullptr) return false;
    const int64_t clipStart = clip->timelineStart;
    const int64_t clipEnd = clipStart + clip->length;
    if (playhead <= clipStart || playhead >= clipEnd) return false;
    // D fades in up to the playhead; G fades out from it. The other end keeps
    // whatever fade it had, and the pair can't overlap.
    int64_t newIn = clip->fadeIn;
    int64_t newOut = clip->fadeOut;
    document::FadeShape inShape = clip->fadeInShape;
    document::FadeShape outShape = clip->fadeOutShape;
    if (fadeIn) {
        newIn = playhead - clipStart;
        inShape = shape;
        if (newIn + newOut > clip->length) newOut = clip->length - newIn;
    } else {
        newOut = clipEnd - playhead;
        outShape = shape;
        if (newIn + newOut > clip->length) newIn = clip->length - newOut;
    }
    undo.execute(std::make_unique<editing::SetClipFadeCommand>(
        trackId, clip->id, newIn, inShape, newOut, outShape));
    return true;
}

int64_t parseTimecodeToSamples(
    const char* text, TimecodeMode mode, double sr, double fps,
    const std::vector<document::TempoChange>* tempo,
    const std::vector<document::TimeSignature>* meter) {
    if (text == nullptr) return -1;
    switch (mode) {
        case TimecodeMode::MinSec: {
            int mm; double ss;
            if (std::sscanf(text, "%d:%lf", &mm, &ss) == 2)
                return static_cast<int64_t>((mm * 60 + ss) * sr);
            break;
        }
        case TimecodeMode::Smpte: {
            int hh, mm, ss2, ff;
            if (std::sscanf(text, "%d:%d:%d:%d", &hh, &mm, &ss2, &ff) == 4)
                return static_cast<int64_t>(
                    (hh * 3600 + mm * 60 + ss2 + ff / fps) * sr);
            break;
        }
        case TimecodeMode::BarsBeats: {
            int bars = 1, beats = 1, ticks = 0;
            const bool parsed =
                std::sscanf(text, "%d|%d|%d", &bars, &beats, &ticks) == 3 ||
                std::sscanf(text, "%d.%d.%d", &bars, &beats, &ticks) == 3;
            if (parsed && tempo != nullptr && meter != nullptr)
                return document::sampleAtBarsBeats(
                    document::BarsBeats{bars, beats, ticks}, sr, *tempo, *meter);
            break;
        }
        case TimecodeMode::FeetFrames: {
            int feet, ff2;
            if (std::sscanf(text, "%d+%d", &feet, &ff2) == 2)
                return static_cast<int64_t>((feet * 16 + ff2) / fps * sr);
            break;
        }
        case TimecodeMode::Samples: {
            long long s;
            if (std::sscanf(text, "%lld", &s) == 1) return s;
            break;
        }
    }
    return -1;
}

void applyClipSelection(TimelineViewState& view, const document::Edit& edit,
                        int trackIndex, const std::string& clipId, bool shift,
                        bool cmd) {
    const auto& tracks = edit.tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return;
    const auto& track = tracks[static_cast<size_t>(trackIndex)];
    auto extentOf = [&](const std::string& id, int64_t& s, int64_t& e) {
        for (const auto& c : track.clips)
            if (c.id == id) { s = c.timelineStart; e = s + c.length; return true; }
        for (const auto& c : track.midiClips)
            if (c.id == id) { s = c.timelineStart; e = s + c.length; return true; }
        return false;
    };
    int64_t clickStart = 0, clickEnd = 0;
    if (!extentOf(clipId, clickStart, clickEnd)) return;

    if (shift) {
        // Range from the anchor (the current primary) to the clicked clip,
        // gaps included; every clip touching that span joins the selection.
        int64_t aStart = clickStart, aEnd = clickEnd;
        extentOf(view.selectedClipId, aStart, aEnd);
        const int64_t rs = std::min(aStart, clickStart);
        const int64_t re = std::max(aEnd, clickEnd);
        view.selectedClipIds.clear();
        for (const auto& c : track.clips)
            if (c.timelineStart < re && c.timelineStart + c.length > rs)
                view.selectedClipIds.insert(c.id);
        for (const auto& c : track.midiClips)
            if (c.timelineStart < re && c.timelineStart + c.length > rs)
                view.selectedClipIds.insert(c.id);
        view.selectionStart = rs;
        view.selectionEnd = re;
        view.selectionRow = trackIndex;
        view.selectionRowEnd = trackIndex;
        view.hasSelection = true;
    } else if (cmd) {
        // Toggle this clip alone — no range, so no gaps are highlighted.
        if (view.selectedClipIds.count(clipId)) view.selectedClipIds.erase(clipId);
        else view.selectedClipIds.insert(clipId);
    } else {
        view.selectedClipIds.clear();
        view.selectedClipIds.insert(clipId);
    }
    view.selectedClipId = clipId;
    view.selectedTrackIndex = trackIndex;
}

bool adjustSelectedTrackHeight(const document::Edit& edit,
                               TimelineViewState& view, float factor) {
    if (view.selectedTrackIndex < 0) return false;
    const auto& tracks = edit.tracks();
    if (view.selectedTrackIndex >= static_cast<int>(tracks.size())) return false;
    const std::string& id = tracks[static_cast<size_t>(view.selectedTrackIndex)].id;
    const auto it = view.trackHeightScales.find(id);
    const float current = it != view.trackHeightScales.end() ? it->second : 1.0f;
    view.trackHeightScales[id] = std::clamp(current * factor, 0.5f, 4.0f);
    return true;
}

bool createCrossfadeForSelectedClip(const document::Edit& edit,
                                    editing::UndoStack& undo,
                                    TimelineViewState& view,
                                    document::FadeShape crossShape,
                                    int64_t freeFadeSamples,
                                    document::FadeShape freeShape) {
    std::string trackId;
    const document::AudioClip* s = selectedAudioClip(edit, view, trackId);
    if (s == nullptr) return false;
    const int64_t sStart = s->timelineStart;
    const int64_t sEnd = sStart + s->length;

    // The first same-track clip whose extent overlaps the selected one.
    const document::AudioClip* n = nullptr;
    for (const auto& tr : edit.tracks()) {
        if (tr.id != trackId) continue;
        for (const auto& c : tr.clips) {
            if (c.id == s->id) continue;
            const int64_t cs = c.timelineStart;
            const int64_t ce = cs + c.length;
            if (cs < sEnd && ce > sStart) { n = &c; break; }
        }
        break;
    }
    if (n == nullptr) return false;  // no overlap — caller does the normal fade

    const int64_t nStart = n->timelineStart;
    const int64_t nEnd = nStart + n->length;
    const int64_t os = std::max(sStart, nStart);
    const int64_t oe = std::min(sEnd, nEnd);
    const int64_t overlap = oe - os;
    if (overlap <= 0) return false;

    // Outgoing ends first, incoming starts later. If they are the same clip the
    // overlap is interior (one clip inside the other) — no clean edge crossfade.
    const document::AudioClip* outgoing = sEnd <= nEnd ? s : n;
    const document::AudioClip* incoming = sStart >= nStart ? s : n;
    if (outgoing == incoming) return false;

    // Crossfade over the whole overlap: the fades are anchored to the outgoing
    // clip's end and the incoming clip's start, which for an edge overlap are
    // exactly the overlap bounds — so equal length makes them line up.
    int64_t outIn = outgoing->fadeIn, outOut = overlap;
    int64_t inIn = overlap, inOut = incoming->fadeOut;
    document::FadeShape outInS = outgoing->fadeInShape, outOutS = crossShape;
    document::FadeShape inInS = crossShape, inOutS = incoming->fadeOutShape;

    // The selected clip's free (non-overlapping) edge gets a normal fade.
    const int64_t freeLen = std::max<int64_t>(0, freeFadeSamples);
    if (s == outgoing) {
        outIn = std::min(freeLen, s->length - outOut);
        outInS = freeShape;
    } else {  // s == incoming
        inOut = std::min(freeLen, s->length - inIn);
        inOutS = freeShape;
    }
    // A clip's two fades can't overlap.
    auto clampPair = [](int64_t& in, int64_t& out, int64_t len) {
        in = std::clamp<int64_t>(in, 0, len);
        out = std::clamp<int64_t>(out, 0, len);
        if (in + out > len) out = len - in;
    };
    clampPair(outIn, outOut, outgoing->length);
    clampPair(inIn, inOut, incoming->length);

    std::vector<std::unique_ptr<editing::Command>> cmds;
    cmds.push_back(std::make_unique<editing::SetClipFadeCommand>(
        trackId, outgoing->id, outIn, outInS, outOut, outOutS));
    cmds.push_back(std::make_unique<editing::SetClipFadeCommand>(
        trackId, incoming->id, inIn, inInS, inOut, inOutS));
    undo.execute(std::make_unique<editing::CompoundCommand>(std::move(cmds),
                                                            "Crossfade"));
    return true;
}

bool createFadeFromSelection(const document::Edit& edit,
                             editing::UndoStack& undo, TimelineViewState& view,
                             document::FadeShape inShape,
                             document::FadeShape outShape,
                             int64_t defaultFadeSamples) {
    if (!view.hasSelection) return false;
    const int64_t selStart = std::min(view.selectionStart, view.selectionEnd);
    const int64_t selEnd = std::max(view.selectionStart, view.selectionEnd);
    if (selEnd <= selStart) return false;

    const auto members = clipsInSelection(edit, view);
    const auto& tracks = edit.tracks();

    std::vector<std::unique_ptr<editing::Command>> commands;
    for (const auto& member : members) {
        if (member.midi) continue;  // fades are audio-only for now
        // Read the clip's current state (the mutation happens in the command).
        const document::AudioClip* clip = nullptr;
        for (const auto& t : tracks) {
            if (t.id != member.trackId) continue;
            for (const auto& c : t.clips) {
                if (c.id == member.clipId) { clip = &c; break; }
            }
            break;
        }
        if (clip == nullptr || clip->length <= 0) continue;

        const int64_t clipStart = clip->timelineStart;
        const int64_t clipEnd = clipStart + clip->length;
        const int64_t ovStart = std::max(selStart, clipStart);
        const int64_t ovEnd = std::min(selEnd, clipEnd);
        if (ovEnd <= ovStart) continue;

        // The overlap is already clamped to the clip, so reaching an edge just
        // means the selection ran to (or past) it — within a few pixels, so a
        // selection that rounds a hair inside still counts as reaching it and a
        // whole-clip selection top-and-tails.
        const int64_t edgeTol =
            static_cast<int64_t>(view.samplesPerPixel * 4.0);
        const bool atHead = ovStart <= clipStart + edgeTol;
        const bool atTail = ovEnd >= clipEnd - edgeTol;

        int64_t newFadeIn = clip->fadeIn;
        int64_t newFadeOut = clip->fadeOut;
        document::FadeShape newInShape = clip->fadeInShape;
        document::FadeShape newOutShape = clip->fadeOutShape;

        if (atHead && atTail) {
            // Whole clip selected: top-and-tail with the default length rather
            // than turning the clip into one long ramp.
            const int64_t half = clip->length / 2;
            const int64_t len = std::clamp<int64_t>(defaultFadeSamples, 0, half);
            newFadeIn = len;
            newFadeOut = len;
            newInShape = inShape;
            newOutShape = outShape;
        } else {
            bool doIn = atHead;
            if (!atHead && !atTail) {
                // Interior range: the nearer edge owns the fade.
                doIn = (ovStart - clipStart) <= (clipEnd - ovEnd);
            }
            if (doIn) {
                newFadeIn = ovEnd - clipStart;
                newInShape = inShape;
            } else {
                newFadeOut = clipEnd - ovStart;
                newOutShape = outShape;
            }
        }

        // A fade-in and a fade-out cannot overlap. Clamp each to the clip, then
        // give the edge the gesture just set priority over the other.
        newFadeIn = std::clamp<int64_t>(newFadeIn, 0, clip->length);
        newFadeOut = std::clamp<int64_t>(newFadeOut, 0, clip->length);
        if (newFadeIn + newFadeOut > clip->length) {
            if (atTail && !atHead) {
                newFadeIn = clip->length - newFadeOut;
            } else {
                newFadeOut = clip->length - newFadeIn;
            }
        }

        commands.push_back(std::make_unique<editing::SetClipFadeCommand>(
            member.trackId, member.clipId, newFadeIn, newInShape, newFadeOut,
            newOutShape));
    }

    if (commands.empty()) return false;
    if (commands.size() == 1) {
        undo.execute(std::move(commands.front()));
    } else {
        undo.execute(std::make_unique<editing::CompoundCommand>(
            std::move(commands), "Set Fades"));
    }
    return true;
}

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
    auto snapPosition = [&](int64_t sample) -> int64_t {
        sample = std::max<int64_t>(0, sample);
        if (!view.snapEnabled) return sample;
        if (view.snapIncrementSamples > 0) {
            const int64_t inc = view.snapIncrementSamples;
            return ((sample + inc / 2) / inc) * inc;
        }
        return snapSampleToFormat(sample, view.tcMode, samplesPerPixel,
                                  static_cast<double>(edit.sampleRate()),
                                  24.0, edit.tempoBpm(), &edit.meterMap(),
                                  &edit.tempoMap());
    };

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
            marker.position = snapPosition(transport.position());
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
            renderPos = snapPosition(
                renderPos + static_cast<int64_t>(dxPx * samplesPerPixel));
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
                        int64_t newPos = snapPosition(static_cast<int64_t>(
                            std::max(0.0, scrollSamples +
                                              localX * samplesPerPixel)));
                        undo.execute(std::make_unique<editing::MoveMarkerCommand>(
                            mt.id, m.id, newPos, m.length));
                        break;
                    }
                }
            }
            // The clip stays selected after a click, move or trim — that is what
            // Cmd+D, Cmd+M and the clip inspector all act on. Only the drag
            // bookkeeping is reset here.
            view.dragKind = TimelineViewState::DragKind::None;
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
            int64_t pos = snapPosition(static_cast<int64_t>(
                std::max(0.0, scrollSamples + localX * samplesPerPixel)));
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
        int64_t clickPos = snapPosition(static_cast<int64_t>(
            std::max(0.0, scrollSamples + localX * samplesPerPixel)));
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
    auto snapPosition = [&](int64_t sample) -> int64_t {
        sample = std::max<int64_t>(0, sample);
        if (!view.snapEnabled) return sample;
        if (view.snapIncrementSamples > 0) {
            const int64_t inc = view.snapIncrementSamples;
            return ((sample + inc / 2) / inc) * inc;
        }
        return snapSampleToFormat(sample, view.tcMode, samplesPerPixel,
                                  static_cast<double>(edit.sampleRate()),
                                  24.0, edit.tempoBpm(), &edit.meterMap(),
                                  &edit.tempoMap());
    };

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
            if (!ImGui::GetIO().KeyCtrl && laneHovered && mouse.x >= clipX && mouse.x <= clipX + clipW &&
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
            int64_t newPos = snapPosition(static_cast<int64_t>(
                std::max(0.0, scrollSamples + localX * samplesPerPixel)));
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
            int64_t seekTo = snapPosition(static_cast<int64_t>(
                std::max(0.0, scrollSamples + localX * samplesPerPixel)));
            transport.seek(seekTo);
        }
    }

    return laneHeight;
}

} // namespace dave::gui
