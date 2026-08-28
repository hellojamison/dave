// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TrackList.h"

#include "gui/Theme.h"
#include "gui/TrackColorPicker.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace dave::gui {
// An eye, open or struck through. Drawn rather than a glyph so the closed
// state is a slash across the same shape — two different characters would read
// as two different controls.
void drawTrackListEye(ImDrawList* dl, ImVec2 centre, bool open, ImU32 color) {
    // The open eye is two arcs bowing away from each other with a pupil; the
    // shut eye is the SAME lower lid alone, lowered, with lashes. Open and
    // shut are the two states of one eye — not an eye and an eye struck out,
    // which reads as "disabled" rather than "closed".
    //
    // The arcs are struck from centres close to the middle and swept wide, so
    // the lids curve properly. Pushing the centres far apart and taking a
    // narrow sweep off each — the obvious construction — gives two nearly
    // straight lines, which reads as a squint rather than an eye.
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float arcRadius = 7.4f;
    constexpr float arcCentreOffset = 3.6f;
    constexpr float sweep = 1.35f;   // radians either side of vertical

    if (open) {
        dl->PathClear();
        dl->PathArcTo(ImVec2(centre.x, centre.y + arcCentreOffset), arcRadius,
                      -kPi * 0.5f - sweep, -kPi * 0.5f + sweep, 24);
        dl->PathStroke(color, 0, 1.5f);
        dl->PathClear();
        dl->PathArcTo(ImVec2(centre.x, centre.y - arcCentreOffset), arcRadius,
                      kPi * 0.5f - sweep, kPi * 0.5f + sweep, 24);
        dl->PathStroke(color, 0, 1.5f);
        // Big enough to read as a pupil at this size; a dot would look like a
        // full stop between two curves.
        dl->AddCircleFilled(centre, 2.9f, color, 16);
        return;
    }

    // Shut: the lower lid, drawn on its own as a lowered eyelid — the lid the
    // open eye rests on, brought down over a closed eye — plus a few lashes so
    // it reads as shut at a glance rather than as half an open eye.
    dl->PathClear();
    dl->PathArcTo(ImVec2(centre.x, centre.y - arcCentreOffset), arcRadius,
                  kPi * 0.5f - sweep, kPi * 0.5f + sweep, 24);
    dl->PathStroke(color, 0, 1.5f);
    // Three lashes hanging from the lid: one at the low point, two off the
    // shoulders, splaying slightly outward.
    const float lidBottom = centre.y - arcCentreOffset + arcRadius;
    dl->AddLine(ImVec2(centre.x, lidBottom),
                ImVec2(centre.x, lidBottom + 2.8f), color, 1.4f);
    dl->AddLine(ImVec2(centre.x - 4.2f, lidBottom - 0.9f),
                ImVec2(centre.x - 5.6f, lidBottom + 1.4f), color, 1.4f);
    dl->AddLine(ImVec2(centre.x + 4.2f, lidBottom - 0.9f),
                ImVec2(centre.x + 5.6f, lidBottom + 1.4f), color, 1.4f);
}

int trackListRowAt(float y, float listTop, size_t rowCount) {
    if (rowCount == 0) return -1;
    const float offset = y - listTop;
    if (offset < 0.0f) return -1;
    const int row = static_cast<int>(offset / kTrackListRowHeight);
    return row < static_cast<int>(rowCount) ? row : -1;
}

void drawTrackList(const document::Edit& edit, TimelineViewState& view) {
    const auto& pal = theme::palette();
    const auto& tracks = edit.tracks();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;

    size_t hiddenCount = 0;
    for (const auto& track : tracks) {
        if (track.hidden) ++hiddenCount;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
    ImFont* small = theme::fonts().small != nullptr ? theme::fonts().small
                                                    : ImGui::GetFont();
    ImGui::PushFont(small);
    if (hiddenCount > 0) {
        ImGui::Text("%zu OF %zu HIDDEN", hiddenCount, tracks.size());
    } else {
        ImGui::TextUnformatted("ALL VISIBLE");
    }
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    // Reconcile: if the primary row isn't in the multi-selection, a plain
    // single-select happened somewhere else — collapse the set onto it.
    if (view.selectedTrackIndex >= 0 &&
        view.selectedTrackIndex < static_cast<int>(tracks.size())) {
        const std::string& primaryId =
            tracks[static_cast<size_t>(view.selectedTrackIndex)].id;
        if (view.selectedTrackIds.count(primaryId) == 0) {
            view.selectedTrackIds.clear();
            view.selectedTrackIds.insert(primaryId);
            view.trackSelectAnchor = view.selectedTrackIndex;
        }
    } else if (view.selectedTrackIndex < 0) {
        view.selectedTrackIds.clear();
    }

    const ImVec2 listTop = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool windowHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    for (size_t row = 0; row < tracks.size(); ++row) {
        const auto& track = tracks[row];
        const float y = listTop.y + static_cast<float>(row) * kTrackListRowHeight;
        const ImVec2 rowMin(listTop.x, y);
        const ImVec2 rowMax(listTop.x + width, y + kTrackListRowHeight);
        const bool rowHovered = windowHovered && mouse.x >= rowMin.x &&
                                mouse.x <= rowMax.x && mouse.y >= rowMin.y &&
                                mouse.y <= rowMax.y;
        const bool isSelected =
            view.selectedTrackIds.count(track.id) > 0 ||
            view.selectedTrackIndex == static_cast<int>(row);

        if (isSelected) {
            dl->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(pal.trackSelected),
                              2.0f);
        } else if (rowHovered) {
            dl->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(pal.surfaceSoft),
                              2.0f);
        }

        const bool overEye = rowHovered &&
                             mouse.x <= rowMin.x + kTrackListEyeWidth;
        // Main is where everything ends up, so it has no eye to click — the
        // document refuses to hide it and a control that never works is worse
        // than no control.
        const bool hideable = !track.isMain;
        const ImU32 eyeColor = ImGui::GetColorU32(
            !hideable ? pal.textSubtle
                      : (track.hidden ? pal.textMuted
                                      : (overEye ? pal.text : pal.textMuted)));
        if (hideable) {
            drawTrackListEye(dl,
                    ImVec2(rowMin.x + kTrackListEyeWidth * 0.5f,
                           y + kTrackListRowHeight * 0.5f),
                    !track.hidden, eyeColor);
        }

        // The track's colour, then its name — the same two things that
        // identify it on the timeline.
        const ImVec4 swatch =
            trackColorValue(track.color, defaultTrackColor(static_cast<int>(row)));
        const float swatchX = rowMin.x + kTrackListEyeWidth;
        dl->AddRectFilled(ImVec2(swatchX, y + 5.0f),
                          ImVec2(swatchX + 3.0f, y + kTrackListRowHeight - 5.0f),
                          ImGui::GetColorU32(swatch), 1.5f);

        dl->PushClipRect(ImVec2(swatchX + 8.0f, y),
                         ImVec2(rowMax.x - 4.0f, rowMax.y), true);
        dl->AddText(ImVec2(swatchX + 8.0f, y + 3.0f),
                    ImGui::GetColorU32(track.hidden ? pal.textSubtle : pal.text),
                    track.name.c_str());
        dl->PopClipRect();

        if (rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (overEye && hideable) {
                view.requestToggleHiddenTrackId = track.id;
            } else if (ImGui::GetIO().KeyShift && view.trackSelectAnchor >= 0 &&
                       view.trackSelectAnchor < static_cast<int>(tracks.size())) {
                // Shift extends a contiguous range from the anchor (the last
                // plain click) to this row; the clicked row becomes primary but
                // the anchor stays put so the range can be reshaped.
                const int lo = std::min(view.trackSelectAnchor,
                                        static_cast<int>(row));
                const int hi = std::max(view.trackSelectAnchor,
                                        static_cast<int>(row));
                view.selectedTrackIds.clear();
                for (int r = lo; r <= hi; ++r) {
                    view.selectedTrackIds.insert(tracks[static_cast<size_t>(r)].id);
                }
                view.selectedTrackIndex = static_cast<int>(row);
            } else {
                // Plain click: a single track, and a fresh anchor for the next
                // Shift-click.
                view.selectedTrackIds.clear();
                view.selectedTrackIds.insert(track.id);
                view.selectedTrackIndex = static_cast<int>(row);
                view.trackSelectAnchor = static_cast<int>(row);
            }
        }
        if (rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            view.selectedTrackIndex = static_cast<int>(row);
            view.trackListContextId = track.id;
            ImGui::OpenPopup("##trackListContext");
        }

        // A thin separator under every row but the last, so the stack reads as
        // distinct rows rather than one continuous column.
        if (row + 1 < tracks.size()) {
            dl->AddLine(ImVec2(rowMin.x + 4.0f, rowMax.y),
                        ImVec2(rowMax.x - 4.0f, rowMax.y),
                        ImGui::GetColorU32(pal.border));
        }
    }

    if (ImGui::BeginPopup("##trackListContext")) {
        const document::Track* ctx = edit.track(view.trackListContextId);
        if (ctx != nullptr) {
            ImGui::TextDisabled("%s", ctx->name.c_str());
            ImGui::Separator();
            // Main is permanent; deleting it would leave the session with no
            // output bus, so the item is disabled rather than absent.
            if (ImGui::MenuItem("Delete", nullptr, false, !ctx->isMain)) {
                view.requestRemoveTrackId = view.trackListContextId;
            }
        }
        ImGui::EndPopup();
    }

    // The rows are drawn, not laid out with ImGui items, so the window still
    // has to be told how much space they took.
    ImGui::Dummy(ImVec2(width, static_cast<float>(tracks.size()) *
                                   kTrackListRowHeight));
}

} // namespace dave::gui
