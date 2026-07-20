#include "gui/Timeline.h"
#include "editing/Commands.h"
#include "gui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dave::gui {

// Helpers converting Palette ImVec4 -> ImU32 (ImDrawList wants packed colors).
static inline ImU32 C(const ImVec4& v) {
    return IM_COL32(
        int(v.x * 255), int(v.y * 255), int(v.z * 255), int(v.w * 255));
}

const std::vector<PeakBucket>& PeakCache::get(const std::string& assetId,
                                              const std::vector<std::vector<float>>& buffer,
                                              int samplesPerPixel) {
    std::string k = key(assetId, samplesPerPixel);
    auto it = cache_.find(k);
    if (it != cache_.end()) return it->second;

    std::vector<PeakBucket> buckets;
    if (!buffer.empty()) {
        const auto& ch = buffer[0];
        const int64_t total = static_cast<int64_t>(ch.size());
        const int64_t numBuckets = (total + samplesPerPixel - 1) / samplesPerPixel;
        buckets.reserve(static_cast<size_t>(numBuckets));
        for (int64_t b = 0; b < numBuckets; ++b) {
            int64_t start = b * samplesPerPixel;
            int64_t end = std::min(start + samplesPerPixel, total);
            float mn = 1e9f, mx = -1e9f;
            for (int64_t i = start; i < end; ++i) {
                mn = std::min(mn, ch[i]);
                mx = std::max(mx, ch[i]);
            }
            if (mn > mx) { mn = 0; mx = 0; }
            buckets.push_back({mn, mx});
        }
    }
    auto [insIt, _] = cache_.emplace(k, std::move(buckets));
    return insIt->second;
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
    // We reserve a fixed gutter on the left for track names and draw everything
    // (ruler, tracks, clips, waveforms, playhead) with absolute coordinates.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gutterWidth = 132.0f;       // a touch wider for track names + meter
    const float totalWidth = avail.x;
    const float totalHeight = avail.y;
    const auto& pal = theme::palette();

    // Reserve the marker lane FIRST so its "Add marker track" button gets
    // interaction space before the timeline's big InvisibleButton claims the
    // whole region. The marker lane returns how much vertical space it used.
    const float markerLaneHeight = drawMarkerLane(edit, undo, transport, view,
        ImVec2(origin.x, origin.y + timelineHeight), totalWidth, gutterWidth,
        view.scrollSamples, view.samplesPerPixel);

    // Reserve the TRACK ROWS area as an invisible interaction widget. This
    // must NOT cover the ruler or marker lane, or it eats their clicks.
    // We account for the cursor advance the marker lane already did.
    const float tracksRegionHeight = totalHeight - timelineHeight - markerLaneHeight;
    char areaBtn[32];
    std::snprintf(areaBtn, sizeof(areaBtn), "##timeline_area_%p", &view);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + timelineHeight + markerLaneHeight));
    ImGui::InvisibleButton(areaBtn, ImVec2(totalWidth, tracksRegionHeight));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool areaHovered = ImGui::IsItemHovered();

    // Is the mouse over the ruler region? (Used for hover highlight + seek.)
    const bool rulerHovered = areaHovered &&
        mouse.y >= origin.y && mouse.y <= origin.y + timelineHeight &&
        mouse.x >= origin.x + gutterWidth;

    // Gutter column background (runs full height, slightly distinct).
    dl->AddRectFilled(origin, ImVec2(origin.x + gutterWidth, origin.y + totalHeight),
                      C(pal.bgElevated));

    // Ruler background + ticks. Brighten slightly when hovered/clickable.
    ImVec4 rulerBg = rulerHovered ? pal.bgElevated : pal.bgAlt;
    dl->AddRectFilled(ImVec2(origin.x + gutterWidth, origin.y),
                      ImVec2(origin.x + totalWidth, origin.y + timelineHeight),
                      C(rulerBg));
    dl->AddLine(ImVec2(origin.x + gutterWidth, origin.y + timelineHeight),
                ImVec2(origin.x + totalWidth, origin.y + timelineHeight),
                C(pal.border));
    const double sr = 48000.0;
    const int64_t secStep = static_cast<int64_t>(sr);
    int64_t firstSec = static_cast<int64_t>(view.scrollSamples / secStep);
    for (int64_t s = firstSec; ; ++s) {
        double x = origin.x + gutterWidth +
            (s * secStep - view.scrollSamples) / view.samplesPerPixel;
        if (x > origin.x + totalWidth) break;
        if (x < origin.x + gutterWidth) continue;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + timelineHeight),
                    C(pal.border));
        char label[16];
        std::snprintf(label, sizeof(label), "%llds", static_cast<long long>(s));
        dl->AddText(ImVec2(x + 4, origin.y + 6), C(pal.textMuted), label);
    }

    const auto& tracks = edit.tracks();
    // Marker lane was already drawn above (before the InvisibleButton). Reuse
    // its height to compute the track-row region.
    const float tracksTop = origin.y + timelineHeight + markerLaneHeight;
    const float tracksAreaHeight = totalHeight - timelineHeight - markerLaneHeight;

    // Tracks + clips.
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const auto& track = tracks[ti];
        float y = tracksTop + static_cast<float>(ti) * trackHeight;
        if (y > origin.y + totalHeight) break;

        bool selected = view.selectedTrackIndex == static_cast<int>(ti);
        // Alternating row backgrounds for readability.
        ImVec4 rowBg = (ti % 2 == 0) ? pal.bg : pal.bgAlt;
        dl->AddRectFilled(ImVec2(origin.x + gutterWidth, y),
                          ImVec2(origin.x + totalWidth, y + trackHeight), C(rowBg));
        // Selected highlight on the track lane.
        if (selected) {
            dl->AddRectFilled(ImVec2(origin.x + gutterWidth, y),
                              ImVec2(origin.x + totalWidth, y + trackHeight),
                              C(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.08f)));
        }
        // Track separator.
        dl->AddLine(ImVec2(origin.x, y + trackHeight),
                    ImVec2(origin.x + totalWidth, y + trackHeight),
                    C(pal.border));
        // If selected, highlight the gutter with an accent stripe so it's
        // obvious which track the Plugins panel is targeting.
        if (selected) {
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + 4, y + trackHeight),
                              C(pal.accent));
        }
        // Track header (in the gutter).
        dl->AddText(ImVec2(origin.x + 10, y + 9),
                    C(pal.text), track.name.c_str());

        // Click the gutter (track header) to select this track. This matters
        // for empty tracks — they have no clip to click, so without this the
        // Plugins panel ("select a track...") could never target them.
        if (areaHovered &&
            mouse.x >= origin.x && mouse.x <= origin.x + gutterWidth &&
            mouse.y >= y && mouse.y <= y + trackHeight &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.selectedTrackIndex = static_cast<int>(ti);
        }
        // Mini gain meter placeholder in the header.
        char gainLabel[16];
        std::snprintf(gainLabel, sizeof(gainLabel), "%.1f dB",
                      20.0f * std::log10(std::max(0.0001f, float(track.gain))));
        dl->AddText(ImVec2(origin.x + 10, y + trackHeight - 16),
                    C(pal.textMuted), gainLabel);

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
            // Clip body: blue, brighter if selected.
            ImVec4 body = pal.clipAudio;
            if (isSel) body = ImVec4(body.x + 0.1f, body.y + 0.08f, body.z + 0.05f, 1.0f);
            dl->AddRectFilled(clipRect.min, clipRect.max, C(body), 2.0f);
            dl->AddRect(clipRect.min, clipRect.max,
                        isSel ? C(pal.accent) : C(pal.clipAudioBorder), 2.0f);

            // Waveform (lighter than the body so it reads).
            auto bufIt = assetBuffers.find(clip.asset.sha256);
            if (bufIt != assetBuffers.end()) {
                int spp = std::max(1, static_cast<int>(view.samplesPerPixel));
                const auto& pk = peaks.get(clip.asset.sha256, bufIt->second, spp);
                double midY = y + trackHeight * 0.5f;
                float halfH = (trackHeight - 12) * 0.45f;
                int64_t startBucket = clip.sourceOffset / spp;
                int64_t numDraw = static_cast<int64_t>(clipW);
                ImU32 waveCol = IM_COL32(220, 235, 255, 230);
                for (int64_t px = 0; px < numDraw; ++px) {
                    int64_t bucket = startBucket + px;
                    if (bucket < 0 || bucket >= static_cast<int64_t>(pk.size())) continue;
                    const auto& b = pk[bucket];
                    float pxX = static_cast<float>(clipX + px);
                    dl->AddLine(ImVec2(pxX, midY - b.max * halfH),
                                ImVec2(pxX, midY - b.min * halfH), waveCol);
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
                    clip.timelineStart = std::max<int64_t>(0, view.dragClipOriginalStart + dxSamples);
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
        transport.seek(seekTo);
    };

    // Ruler: click seeks; drag scrubs (continuous seek while held).
    if (rulerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        seekToMouseX();
    }
    if (rulerHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        seekToMouseX();
    }

    // Track area: click empty space seeks (but not on clips — those drag).
    if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.dragging) {
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
            seekToMouseX();
        }
    }

    // Playhead — amber, full height, with a small cap triangle at the top.
    int64_t pos = transport.position();
    double playheadX = origin.x + gutterWidth +
        (pos - view.scrollSamples) / view.samplesPerPixel;
    if (playheadX >= origin.x + gutterWidth && playheadX <= origin.x + totalWidth) {
        dl->AddLine(ImVec2(playheadX, origin.y),
                    ImVec2(playheadX, origin.y + totalHeight),
                    C(pal.accent), 2.0f);
        // Cap.
        dl->AddTriangleFilled(
            ImVec2(playheadX - 5, origin.y),
            ImVec2(playheadX + 5, origin.y),
            ImVec2(playheadX, origin.y + 7), C(pal.accent));
    }

    // Video lane — a strip at the bottom of the timeline showing video clips.
    // Placed below the track rows, above the scroll/zoom handler.
    {
        float videoLaneY = tracksTop + static_cast<float>(tracks.size()) * trackHeight;
        drawVideoLane(edit, transport, view,
                      ImVec2(origin.x, videoLaneY),
                      totalWidth, gutterWidth,
                      view.scrollSamples, view.samplesPerPixel);
    }

    // Scroll / zoom.
    double wheel = ImGui::GetIO().MouseWheel;
    if (areaHovered && wheel != 0.0) {
        if (ImGui::GetIO().KeyCtrl) {
            view.samplesPerPixel = std::clamp(view.samplesPerPixel - wheel * 10.0, 4.0, 50000.0);
        } else {
            view.scrollSamples = std::max(0.0, view.scrollSamples - wheel * view.samplesPerPixel * 20.0);
        }
    }
}

// ─── Marker lane ────────────────────────────────────────────────────────────

namespace {

// Default color per marker kind (used when Marker::color is empty).
ImU32 markerColor(document::MarkerKind k) {
    switch (k) {
        case document::MarkerKind::Cue:     return IM_COL32(245, 166,  35, 255); // amber
        case document::MarkerKind::Section: return IM_COL32(180,  90, 220, 255); // purple
        case document::MarkerKind::Loop:    return IM_COL32( 80, 200, 120, 255); // green
        case document::MarkerKind::Punch:   return IM_COL32(220,  80,  80, 255); // red
        case document::MarkerKind::CD:      return IM_COL32( 90, 160, 220, 255); // blue
        default:                            return IM_COL32(160, 160, 170, 255); // grey (Custom)
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

    // No video tracks → hint.
    if (edit.videoTracks().empty()) {
        dl->AddText(ImVec2(origin.x + gutterWidth + 8, origin.y + 6),
                    C(pal.textMuted), "(no video — File > Load Video)");
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

            // Clip block — dark teal body (distinct from blue audio clips).
            ImU32 bodyCol = IM_COL32(45, 90, 85, 255);
            ImU32 borderCol = IM_COL32(80, 140, 130, 255);
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
            ++clipCount;
        }
    }

    // Click empty lane area to seek; click a clip to select.
    if (laneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
                    view.selectedClipId = clip.id;
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
