// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/Mixer.h"

#include "editing/Commands.h"
#include "gui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dave::gui {

namespace {

ImU32 C(const ImVec4& v) {
    return IM_COL32(int(v.x * 255), int(v.y * 255), int(v.z * 255), int(v.w * 255));
}

constexpr float kInsertRowHeight = 17.0f;
constexpr float kRowGap = 2.0f;
constexpr float kSectionGap = 6.0f;
// Enough of the strip is fixed-height that the fader is what absorbs a resize.
// Below this the fader stops being usable as a fader, so the strip clips
// instead of shrinking it further.
constexpr float kMinFaderHeight = 60.0f;

// One track's mixer-visible state. Pointers rather than a copy because a strip
// edits in place, and a reference-holding struct is what lets audio and MIDI
// tracks — identical from the mixer's point of view except for the instrument
// row — share one implementation.
struct StripModel {
    std::string trackId;
    std::string* name = nullptr;
    double* gain = nullptr;
    double* pan = nullptr;
    bool* mute = nullptr;
    bool* solo = nullptr;
    std::vector<document::PluginSlot>* inserts = nullptr;
    // Null for an audio track. Non-null (though possibly empty) for MIDI.
    document::PluginSlot* instrument = nullptr;
    bool isMidi = false;
};

// What a strip decided to do, applied by the caller once the loop over strips
// has finished. Mutating the track vectors mid-loop would invalidate the very
// references the remaining strips are drawn from.
struct StripAction {
    enum class Kind { None, AddInsert, RemoveInsert, ClearInstrument };
    Kind kind = Kind::None;
    std::string trackId;
    std::string slotId;
    bool isMidi = false;
};

// A compact slot button: bypass dot on the left, name filling the rest.
// Left-click opens the plugin's editor, right-click opens a menu. Returns true
// if the row wants to be removed.
bool drawSlotRow(int uid, const char* label, document::PluginSlot& slot,
                 document::Edit& edit, TimelineViewState& view, float width,
                 bool isInstrument) {
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(uid);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, kInsertRowHeight);
    const bool pressed = ImGui::InvisibleButton("##slot", size);
    const bool hovered = ImGui::IsItemHovered();

    if (pressed) view.requestPluginEditorSlotId = slot.id;

    bool remove = false;
    if (ImGui::BeginPopupContextItem("##slotmenu")) {
        if (ImGui::MenuItem("Open Editor")) view.requestPluginEditorSlotId = slot.id;
        if (ImGui::MenuItem("Bypass", nullptr, slot.bypass)) {
            slot.bypass = !slot.bypass;
            edit.notifyChanged();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(isInstrument ? "Clear Instrument" : "Remove Insert")) {
            remove = true;
        }
        ImGui::EndPopup();
    }

    const ImVec2 bMin = pos;
    const ImVec2 bMax(pos.x + size.x, pos.y + size.y);
    const ImVec4 accentColor = isInstrument ? pal.clipMidiBorder : pal.accent;
    dl->AddRectFilled(bMin, bMax,
                      hovered ? C(pal.surfaceStrong) : C(pal.trackControlInactive),
                      2.0f);
    dl->AddRect(bMin, bMax, C(slot.bypass ? pal.border : accentColor), 2.0f);

    // A bypassed insert still occupies its place in the chain — it has to stay
    // visible and in order, or bypassing would look like removing.
    const float dotR = 3.0f;
    const ImVec2 dotC(bMin.x + 7.0f, (bMin.y + bMax.y) * 0.5f);
    dl->AddCircleFilled(dotC, dotR,
                        C(slot.bypass ? pal.textSubtle : pal.success));

    dl->PushClipRect(ImVec2(bMin.x + 13.0f, bMin.y),
                     ImVec2(bMax.x - 3.0f, bMax.y), true);
    dl->AddText(ImVec2(bMin.x + 14.0f, bMin.y + 1.0f),
                C(slot.bypass ? pal.textSubtle : pal.text),
                slot.name.empty() ? label : slot.name.c_str());
    dl->PopClipRect();

    if (hovered) {
        ImGui::SetTooltip("%s%s\nClick to open, right-click for options",
                          slot.name.c_str(), slot.bypass ? " (bypassed)" : "");
    }
    ImGui::PopID();
    return remove;
}

// An empty row that adds the next insert. Deliberately at the END of the
// chain rather than a fixed grid of empty slots: the chain is a list that
// grows, and a "+" that always sits after the last insert says where the new
// plugin lands in the signal path.
bool drawAddInsertRow(int uid, float width) {
    const auto& pal = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(uid);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed =
        ImGui::InvisibleButton("##addinsert", ImVec2(width, kInsertRowHeight));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 bMin = pos;
    const ImVec2 bMax(pos.x + width, pos.y + kInsertRowHeight);
    if (hovered) dl->AddRectFilled(bMin, bMax, C(pal.surfaceStrong), 2.0f);
    // Dashed, like the timeline's "+ Add track": an affordance, not a control.
    constexpr float dash = 4.0f;
    constexpr float gap = 3.0f;
    const ImU32 col = C(hovered ? pal.accent : pal.borderStrong);
    for (float x = bMin.x; x < bMax.x; x += dash + gap) {
        const float x2 = std::min(x + dash, bMax.x);
        dl->AddLine(ImVec2(x, bMin.y), ImVec2(x2, bMin.y), col);
        dl->AddLine(ImVec2(x, bMax.y), ImVec2(x2, bMax.y), col);
    }
    const ImVec2 ts = ImGui::CalcTextSize("+");
    dl->AddText(ImVec2((bMin.x + bMax.x - ts.x) * 0.5f, bMin.y + 1.0f),
                C(hovered ? pal.accentStrong : pal.textMuted), "+");
    if (hovered) ImGui::SetTooltip("Add an insert to the end of the chain");
    ImGui::PopID();
    return pressed;
}

StripAction drawStrip(const StripModel& m, document::Edit& edit,
                      TimelineViewState& view, int uid, bool selected,
                      bool anySoloed, float stripWidth, float stripHeight) {
    const auto& pal = theme::palette();
    StripAction action;
    ImGui::PushID(uid);

    ImGui::BeginChild("##strip", ImVec2(stripWidth, stripHeight), true,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    // A strip stacks a dozen items, and the theme's default vertical item
    // spacing adds up to more than the fader's whole height budget — which is
    // what pushed the fader off the bottom of the strip. Sections are spaced
    // deliberately below instead.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 stripMin = ImGui::GetWindowPos();
    const ImVec2 stripMax(stripMin.x + ImGui::GetWindowWidth(),
                          stripMin.y + ImGui::GetWindowHeight());
    if (selected) {
        dl->AddRectFilled(stripMin, stripMax,
                          IM_COL32(int(pal.accent.x * 255), int(pal.accent.y * 255),
                                   int(pal.accent.z * 255), 20));
    }

    const float inner = ImGui::GetContentRegionAvail().x;

    // ─── Name ───────────────────────────────────────────────────────────────
    // The colour bar reuses the timeline's identity colours so a strip and its
    // row are recognisably the same track.
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(pos.x, pos.y),
                          ImVec2(pos.x + inner, pos.y + 3.0f),
                          C(m.isMidi ? pal.clipMidiBorder : pal.clipAudioBorder));
        ImGui::Dummy(ImVec2(inner, 4.0f));
    }
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##name", ImVec2(inner, 16.0f))) {
            view.selectedTrackIndex = uid;
        }
        dl->PushClipRect(pos, ImVec2(pos.x + inner, pos.y + 16.0f), true);
        dl->AddText(pos, C(selected ? pal.accentStrong : pal.text),
                    m.name->c_str());
        dl->PopClipRect();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.name->c_str());
    }
    ImGui::Dummy(ImVec2(inner, kSectionGap * 0.5f));

    // ─── Instrument (MIDI only) ─────────────────────────────────────────────
    // Ids below only need to be unique WITHIN the strip: PushID(uid) above
    // already scopes the whole thing to this track.
    if (m.instrument != nullptr) {
        ImGui::TextDisabled("INST");
        if (m.instrument->uidString.empty()) {
            // An empty instrument slot picks an instrument, not an effect.
            if (drawAddInsertRow(1, inner)) {
                view.requestPicker =
                    TimelineViewState::PluginPicker::MidiInstrument;
                view.requestPickerTrackId = m.trackId;
            }
        } else if (drawSlotRow(2, "Instrument", *m.instrument, edit, view,
                               inner, true)) {
            action = StripAction{StripAction::Kind::ClearInstrument, m.trackId,
                                 m.instrument->id, true};
        }
        ImGui::Dummy(ImVec2(inner, kSectionGap * 0.5f));
    }

    // ─── Inserts ────────────────────────────────────────────────────────────
    ImGui::TextDisabled("INSERTS");
    int insertIdx = 0;
    for (auto& slot : *m.inserts) {
        char label[16];
        std::snprintf(label, sizeof(label), "Insert %d", insertIdx + 1);
        if (drawSlotRow(100 + insertIdx, label, slot, edit, view, inner, false)) {
            action = StripAction{StripAction::Kind::RemoveInsert, m.trackId,
                                 slot.id, m.isMidi};
        }
        ImGui::Dummy(ImVec2(inner, kRowGap));
        ++insertIdx;
    }
    if (drawAddInsertRow(3, inner)) {
        action = StripAction{StripAction::Kind::AddInsert, m.trackId, "",
                             m.isMidi};
    }

    // ─── Mute / solo ────────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(inner, kSectionGap));
    {
        const float half = (inner - 4.0f) * 0.5f;
        struct Toggle { const char* label; bool* flag; ImVec4 active; };
        const Toggle toggles[2] = {
            {"M", m.mute, pal.trackMuteActive},
            {"S", m.solo, pal.trackSoloActive},
        };
        for (int b = 0; b < 2; ++b) {
            ImGui::PushID(b);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##ms", ImVec2(half, 18.0f))) {
                *toggles[b].flag = !*toggles[b].flag;
                edit.notifyChanged();
            }
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 bMin = pos;
            const ImVec2 bMax(pos.x + half, pos.y + 18.0f);
            const bool on = *toggles[b].flag;
            dl->AddRectFilled(bMin, bMax,
                              on ? C(toggles[b].active)
                                 : (hovered ? C(pal.surfaceStrong)
                                            : C(pal.trackControlInactive)),
                              3.0f);
            dl->AddRect(bMin, bMax, on ? C(toggles[b].active) : C(pal.border), 3.0f);
            const ImVec2 ts = ImGui::CalcTextSize(toggles[b].label);
            dl->AddText(ImVec2((bMin.x + bMax.x - ts.x) * 0.5f,
                               (bMin.y + bMax.y - ts.y) * 0.5f),
                        on ? IM_COL32(32, 30, 28, 255) : C(pal.textMuted),
                        toggles[b].label);
            ImGui::PopID();
            if (b == 0) ImGui::SameLine(0.0f, 4.0f);
        }
    }

    // Someone else's solo is silencing this strip. Same cue as the timeline
    // gutter, for the same reason: an unexplained silent track is the hardest
    // kind of problem to hear your way out of.
    if (!*m.mute && !*m.solo && anySoloed) {
        dl->AddRectFilled(stripMin, stripMax, IM_COL32(20, 19, 18, 110));
    }

    // ─── Pan ────────────────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(inner, kRowGap));
    {
        float panVal = static_cast<float>(*m.pan);
        ImGui::SetNextItemWidth(inner);
        // The format string is the readout: ImGui prints it verbatim when it
        // holds no conversion specifier, which "L50" does not.
        const std::string panText = theme::formatPan(panVal);
        if (ImGui::SliderFloat("##pan", &panVal, -1.0f, 1.0f,
                               panText.c_str())) {
            *m.pan = panVal;
            edit.notifyChanged();
        }
        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) {
            *m.pan = 0.0;
            edit.notifyChanged();
        }
    }

    // ─── Fader ──────────────────────────────────────────────────────────────
    // Vertical, because that is what a fader is, and it is what absorbs the
    // panel's spare height. The dB readout underneath is reserved for FIRST:
    // taking the leftovers meant a short panel pushed the readout past the
    // strip's bottom edge and clipped it away.
    {
        const float readoutH = ImGui::GetTextLineHeightWithSpacing();
        const float remaining = ImGui::GetContentRegionAvail().y - readoutH;
        // When the panel really is too short, the fader shrinks rather than
        // overflowing — a stubby fader is usable, a clipped one is not.
        const float faderH = std::max(kMinFaderHeight * 0.4f,
                                      std::min(remaining, kMinFaderHeight * 4.0f));
        float gainDb =
            20.0f * std::log10(std::max(0.0001f, static_cast<float>(*m.gain)));
        constexpr float faderW = 24.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - faderW) * 0.5f);
        if (ImGui::VSliderFloat("##fader", ImVec2(faderW, faderH), &gainDb,
                                -60.0f, 6.0f, "")) {
            *m.gain = std::pow(10.0f, gainDb / 20.0f);
            edit.notifyChanged();
        }
        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) {
            *m.gain = 1.0;
            edit.notifyChanged();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fader (alt-click for 0 dB)");
        char dbText[16];
        std::snprintf(dbText, sizeof(dbText), "%+.1f", gainDb);
        const ImVec2 ts = ImGui::CalcTextSize(dbText);
        ImGui::SetCursorPosX(std::max(0.0f, (inner - ts.x) * 0.5f));
        ImGui::TextUnformatted(dbText);
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopID();
    return action;
}

} // namespace

void drawMixer(document::Edit& edit, editing::UndoStack& undo,
               TimelineViewState& view, float stripWidth) {
    const bool anySoloed = edit.anySoloed();

    if (edit.tracks().empty() && edit.midiTracks().empty()) {
        const auto& pal = theme::palette();
        const char* msg = "No tracks yet";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - ts.x) * 0.5f,
                                   ImGui::GetCursorPosY() + (avail.y - ts.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginChild("##mixerScroll", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // The horizontal scrollbar sits inside this child and eats from the bottom.
    // Sizing strips to the full available height instead pushed each strip's
    // last widget — the fader — under the scrollbar and out of view.
    const float stripHeight = std::max(
        120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize);

    StripAction action;
    int uid = 0;

    // Audio strips, then MIDI, matching the timeline's row order — the mixer
    // and the timeline have to agree on what "the third track" means, since
    // selectedTrackIndex is shared between them.
    for (auto& t : edit.tracksMut()) {
        StripModel m;
        m.trackId = t.id;
        m.name = &t.name;
        m.gain = &t.gain;
        m.pan = &t.pan;
        m.mute = &t.mute;
        m.solo = &t.solo;
        m.inserts = &t.plugins;
        const StripAction a = drawStrip(m, edit, view, uid,
                                        view.selectedTrackIndex == uid,
                                        anySoloed, stripWidth, stripHeight);
        if (a.kind != StripAction::Kind::None) action = a;
        ImGui::SameLine(0.0f, 3.0f);
        ++uid;
    }
    for (auto& t : edit.midiTracksMut()) {
        StripModel m;
        m.trackId = t.id;
        m.name = &t.name;
        m.gain = &t.gain;
        m.pan = &t.pan;
        m.mute = &t.mute;
        m.solo = &t.solo;
        m.inserts = &t.plugins;
        m.instrument = &t.instrument;
        m.isMidi = true;
        const StripAction a = drawStrip(m, edit, view, uid,
                                        view.selectedTrackIndex == uid,
                                        anySoloed, stripWidth, stripHeight);
        if (a.kind != StripAction::Kind::None) action = a;
        ImGui::SameLine(0.0f, 3.0f);
        ++uid;
    }
    ImGui::EndChild();

    // Applied after the loop: every one of these reallocates a track vector or
    // a plugin vector that the strips above were drawn from.
    switch (action.kind) {
        case StripAction::Kind::None:
            break;
        case StripAction::Kind::AddInsert:
            view.requestPicker = action.isMidi
                ? TimelineViewState::PluginPicker::MidiFx
                : TimelineViewState::PluginPicker::AudioFx;
            view.requestPickerTrackId = action.trackId;
            break;
        case StripAction::Kind::RemoveInsert:
            if (action.isMidi) {
                undo.execute(std::make_unique<editing::RemoveMidiPluginCommand>(
                    action.trackId, action.slotId));
            } else {
                undo.execute(std::make_unique<editing::RemovePluginCommand>(
                    action.trackId, action.slotId));
            }
            break;
        case StripAction::Kind::ClearInstrument:
            undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
                action.trackId, document::PluginSlot{}));
            break;
    }
}

} // namespace dave::gui
