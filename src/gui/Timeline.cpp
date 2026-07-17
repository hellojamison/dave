#include "gui/Timeline.h"
#include "editing/Commands.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace dave::gui {

const std::vector<PeakBucket>& PeakCache::get(const std::string& assetId,
                                              const std::vector<std::vector<float>>& buffer,
                                              int samplesPerPixel) {
    std::string k = key(assetId, samplesPerPixel);
    auto it = cache_.find(k);
    if (it != cache_.end()) return it->second;

    // Compute peaks from the first channel (mono or L of stereo) for the strip.
    // A later improvement mixes channels or draws them separately.
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
            if (mn > mx) { mn = 0; mx = 0; } // empty bucket
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
    // We render into a child region that owns horizontal scrolling. The gutter
    // (track names) is a sibling that doesn't scroll horizontally.
    const float gutterWidth = 120.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    // --- Ruler --------------------------------------------------------------
    ImGui::BeginChild("ruler", ImVec2(0, timelineHeight), false,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + timelineHeight),
                          IM_COL32(40, 40, 46, 255));
        // Tick marks every second of audio.
        const double sr = 48000.0; // RB-2 assumes 48k for ruler; real tempo map later
        const int64_t secStep = static_cast<int64_t>(sr);
        int64_t firstSec = static_cast<int64_t>(view.scrollSamples / secStep);
        for (int64_t s = firstSec; ; ++s) {
            double x = p0.x + gutterWidth + (s * secStep - view.scrollSamples) / view.samplesPerPixel;
            if (x > p0.x + avail.x) break;
            if (x < p0.x + gutterWidth) continue;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + timelineHeight),
                        IM_COL32(90, 90, 100, 255));
            char label[16];
            std::snprintf(label, sizeof(label), "%llds", static_cast<long long>(s));
            dl->AddText(ImVec2(x + 4, p0.y + 4), IM_COL32(180, 180, 190, 255), label);
        }
    }
    ImGui::EndChild();

    // --- Gutter (track names) + scrollable clip area ------------------------
    ImGui::BeginChild("tracks", ImVec2(0, 0), false);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();

        const auto& tracks = edit.tracks();
        for (size_t ti = 0; ti < tracks.size(); ++ti) {
            const auto& track = tracks[ti];
            float y = origin.y + static_cast<float>(ti) * trackHeight;
            float x = origin.x;

            // Track background.
            bool selected = view.selectedTrackIndex == static_cast<int>(ti);
            ImU32 bg = selected ? IM_COL32(50, 50, 60, 255) : IM_COL32(32, 32, 38, 255);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + avail.x, y + trackHeight), bg);
            // Gutter label.
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + gutterWidth, y + trackHeight),
                              IM_COL32(24, 24, 28, 255));
            dl->AddText(ImVec2(x + 8, y + trackHeight * 0.5f - 8),
                        IM_COL32(220, 220, 230, 255), track.name.c_str());

            // Clips.
            for (const auto& clip : track.clips) {
                double clipX = x + gutterWidth +
                    (clip.timelineStart - view.scrollSamples) / view.samplesPerPixel;
                double clipW = clip.length / view.samplesPerPixel;
                if (clipW < 2) clipW = 2;
                if (clipX + clipW < x + gutterWidth) continue;
                if (clipX > x + avail.x) continue;

                // Clip body.
                dl->AddRectFilled(ImVec2(clipX, y + 4),
                                  ImVec2(clipX + clipW, y + trackHeight - 4),
                                  IM_COL32(70, 110, 160, 255));
                dl->AddRect(ImVec2(clipX, y + 4),
                            ImVec2(clipX + clipW, y + trackHeight - 4),
                            IM_COL32(120, 160, 210, 255));

                // Waveform inside the clip.
                auto bufIt = assetBuffers.find(clip.asset.sha256);
                if (bufIt != assetBuffers.end()) {
                    int spp = std::max(1, static_cast<int>(view.samplesPerPixel));
                    const auto& pk = peaks.get(clip.asset.sha256, bufIt->second, spp);
                    double midY = y + trackHeight * 0.5f;
                    float halfH = trackHeight * 0.4f;
                    int64_t startBucket = clip.sourceOffset / spp;
                    int64_t numDraw = static_cast<int64_t>(clipW);
                    for (int64_t px = 0; px < numDraw; ++px) {
                        int64_t bucket = startBucket + px;
                        if (bucket < 0 || bucket >= static_cast<int64_t>(pk.size())) continue;
                        const auto& b = pk[bucket];
                        float pxX = static_cast<float>(clipX + px);
                        dl->AddLine(ImVec2(pxX, midY - b.max * halfH),
                                    ImVec2(pxX, midY - b.min * halfH),
                                    IM_COL32(200, 220, 255, 230));
                    }
                }

                // Drag to move the clip.
                ImGui::SetCursorScreenPos(ImVec2(clipX, y + 4));
                char clipBtn[64];
                std::snprintf(clipBtn, sizeof(clipBtn), "##clip_%s", clip.id.c_str());
                ImGui::InvisibleButton(clipBtn,
                    ImVec2(static_cast<float>(clipW), trackHeight - 8));
                if (ImGui::IsItemClicked()) {
                    view.selectedClipId = clip.id;
                    view.selectedTrackIndex = static_cast<int>(ti);
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    // Live preview: move the clip by the drag delta (in samples).
                    // This is a transient visual mutation — the authoritative
                    // move is committed via MoveClipCommand on release. We do
                    // NOT notifyChanged() here (the Edit is const to the view),
                    // so the engine keeps playing the pre-drag position until
                    // the commit. That's acceptable for RB-2.
                    double dx = ImGui::GetMouseDragDelta().x;
                    int64_t deltaSamples = static_cast<int64_t>(dx * view.samplesPerPixel);
                    auto* c = const_cast<document::AudioClip*>(&clip);
                    c->timelineStart = std::max<int64_t>(0, view.dragClipOriginalStart + deltaSamples);
                    view.dragging = true;
                }
                if (ImGui::IsItemActivated()) {
                    view.dragClipOriginalStart = clip.timelineStart;
                    view.dragging = false;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    // Commit the move as an undoable command.
                    auto* c = const_cast<document::AudioClip*>(&clip);
                    undo.execute(std::make_unique<editing::MoveClipCommand>(
                        track.id, clip.id, c->timelineStart));
                    ImGui::ResetMouseDragDelta();
                    view.dragging = false;
                }
            }

            // Click empty track area to seek transport.
            ImGui::SetCursorScreenPos(ImVec2(x + gutterWidth, y));
            char trackBtn[64];
            std::snprintf(trackBtn, sizeof(trackBtn), "##track_%zu", ti);
            ImGui::InvisibleButton(trackBtn, ImVec2(avail.x - gutterWidth, trackHeight));
            if (ImGui::IsItemClicked()) {
                double localX = ImGui::GetMousePos().x - (x + gutterWidth);
                int64_t seekTo = static_cast<int64_t>(
                    view.scrollSamples + localX * view.samplesPerPixel);
                transport.seek(seekTo);
                view.selectedTrackIndex = static_cast<int>(ti);
            }
        }

        // Playhead.
        int64_t pos = transport.position();
        double playheadX = origin.x + gutterWidth +
            (pos - view.scrollSamples) / view.samplesPerPixel;
        if (playheadX >= origin.x + gutterWidth && playheadX <= origin.x + avail.x) {
            dl->AddLine(ImVec2(playheadX, origin.y),
                        ImVec2(playheadX, origin.y + tracks.size() * trackHeight),
                        IM_COL32(255, 220, 80, 255), 2.0f);
        }

        // Horizontal scroll via scroll wheel / drag in empty area.
        double wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0 && ImGui::IsWindowHovered()) {
            view.scrollSamples = std::max(0.0, view.scrollSamples - wheel * view.samplesPerPixel * 20.0);
        }
        // Ctrl+wheel zooms.
        if (ImGui::GetIO().KeyCtrl && wheel != 0.0) {
            view.samplesPerPixel = std::clamp(view.samplesPerPixel - wheel * 10.0, 4.0, 50000.0);
        }
    }
    ImGui::EndChild();
}

} // namespace dave::gui
