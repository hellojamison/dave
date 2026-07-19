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

    // Reserve the full timeline area as an invisible interaction widget so
    // ImGui accounts for the space (and we get hover/click hit-testing).
    char areaBtn[32];
    std::snprintf(areaBtn, sizeof(areaBtn), "##timeline_area_%p", &view);
    ImGui::InvisibleButton(areaBtn, ImVec2(totalWidth, totalHeight));
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
    const float tracksTop = origin.y + timelineHeight;
    const float tracksAreaHeight = totalHeight - timelineHeight;

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
            // Use the clipRect as a hit region via the area button's drag.
            // (Simpler than per-clip invisible buttons given we already have
            //  one big InvisibleButton covering the area.)
            if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && clipHovered) {
                view.selectedClipId = clip.id;
                view.selectedTrackIndex = static_cast<int>(ti);
                view.dragClipOriginalStart = clip.timelineStart;
                view.dragOriginalTrackId = track.id;  // remember source for cross-track move
                view.dragging = true;
            }
        }
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

} // namespace dave::gui
