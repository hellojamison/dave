// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ChannelStrip.h"

#include "editing/Commands.h"
#include "gui/RoutingViewModel.h"
#include "gui/Theme.h"
#include "gui/TrackColorPicker.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace dave::gui {
namespace {

// The strip is a narrow column, so controls are full-width and stacked rather
// than laid out in rows that would wrap.
constexpr float kRowHeight = 22.0f;
constexpr float kSectionGap = 10.0f;
// Every row in the chain is this tall, whatever it holds. The list reads as a
// list because the rows line up, not because they are labelled.
constexpr float kRemoveWidth = 22.0f;
constexpr float kGutter = 4.0f;
// A fader is a vertical gesture read against a scale, so it is drawn as one.
constexpr float kFaderWidth = 26.0f;
constexpr float kFaderHeight = 132.0f;
// Pan is a trim, not the channel's main gesture; at full width it read as the
// more important of the two.
constexpr float kPanWidth = 110.0f;
// The send bar at rest, and while held.
constexpr float kSendFaderWidth = 62.0f;
constexpr float kSendExpandedWidth = 210.0f;
// The meter reads continuously and is the row least in need of a label, so it
// gets the height the labelled rows do not need.
constexpr float kMeterRowHeight = 56.0f;

// A send's level, as a compact bar that grows while you hold it.
//
// A send is one row in the chain, so its fader has to fit on that row — but a
// 60 px bar is far too coarse to set a level with. Holding it expands the bar
// across the strip and doubles its height, drawn over the rows either side,
// so the gesture that needs the resolution is the one that gets it. Pro Tools
// does the same thing for the same reason.
//
// The drag is relative rather than absolute: an expanding control whose value
// jumped to the cursor would move the moment it grew.
bool sendFader(const char* id, float& db, float width, float height) {
    const auto& pal = theme::palette();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##send", ImVec2(width, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    bool changed = false;
    if (active) {
        const float travel = ImGui::GetIO().MouseDelta.x;
        if (travel != 0.0f) {
            // Expanded width sets the resolution, so the value tracks the
            // pointer at the size the user is actually looking at.
            db = std::clamp(db + travel * (66.0f / kSendExpandedWidth),
                            -60.0f, 6.0f);
            changed = true;
        }
    }
    if (theme::altClickedReset()) {
        db = -60.0f;
        changed = true;
    }

    const float drawWidth = active ? kSendExpandedWidth : width;
    const float drawHeight = active ? height * 2.0f : height;
    const float drawX = active
        ? pos.x + width - drawWidth   // grows leftward, staying under the cursor
        : pos.x;
    const float drawY = active ? pos.y - (drawHeight - height) * 0.5f : pos.y;
    ImDrawList* dl = active ? ImGui::GetForegroundDrawList()
                            : ImGui::GetWindowDrawList();
    const ImVec2 barMin(drawX, drawY);
    const ImVec2 barMax(drawX + drawWidth, drawY + drawHeight);
    dl->AddRectFilled(barMin, barMax, ImGui::GetColorU32(pal.surfaceBase), 2.0f);
    const float filled = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
    dl->AddRectFilled(
        barMin, ImVec2(drawX + drawWidth * filled, barMax.y),
        ImGui::GetColorU32(active || hovered ? pal.accentStrong : pal.accent),
        2.0f);
    dl->AddRect(barMin, barMax, ImGui::GetColorU32(pal.border), 2.0f);

    // The number only while it is being set: at rest the bar is the readout,
    // and a value printed on a 60 px bar is unreadable anyway.
    if (active) {
        char text[16];
        std::snprintf(text, sizeof(text), "%+.1f dB", db);
        const ImVec2 size = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(drawX + (drawWidth - size.x) * 0.5f,
                           drawY + (drawHeight - size.y) * 0.5f),
                    ImGui::GetColorU32(pal.primaryText), text);
    }
    if (hovered && !active) {
        ImGui::SetTooltip("Send level %+.1f dB \xe2\x80\x94 drag to set, "
                          "Option-click for off", db);
    }
    ImGui::PopID();
    return changed;
}

// Where the dragged row would land, drawn across the list so a drag in
// progress shows its result instead of only its outcome.
void drawDropLine(float y, float left, float right) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(theme::palette().accent);
    dl->AddLine(ImVec2(left, y), ImVec2(right, y), color, 2.0f);
}

void sectionLabel(const char* text) {
    ImGui::Dummy(ImVec2(0.0f, kSectionGap - ImGui::GetStyle().ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().textSubtle);
    ImFont* font = theme::fonts().small != nullptr ? theme::fonts().small
                                                   : ImGui::GetFont();
    ImGui::PushFont(font);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
}

std::string hardwareInputLabel(const document::HardwareChannelSpan& span) {
    if (span.channelCount <= 0) return "None";
    if (span.channelCount == 2) {
        return "In " + std::to_string(span.firstChannel + 1) + "-" +
               std::to_string(span.firstChannel + 2);
    }
    return "In " + std::to_string(span.firstChannel + 1);
}

// Every hardware input a track can be fed from, mono then stereo pairs — the
// same set the old combo offered, as a list so a glance shows what is
// available instead of what is currently chosen.
std::vector<document::HardwareChannelSpan> inputOptions(int captureChannels) {
    std::vector<document::HardwareChannelSpan> out;
    for (int channel = 0; channel < captureChannels; ++channel) {
        out.push_back({channel, 1});
    }
    for (int channel = 0; channel + 1 < captureChannels; channel += 2) {
        out.push_back({channel, 2});
    }
    return out;
}

double gainToDb(double gain) {
    return gain <= 0.0 ? -60.0 : 20.0 * std::log10(gain);
}

double dbToGain(double db) {
    return db <= -59.9 ? 0.0 : std::pow(10.0, db / 20.0);
}

} // namespace

bool pluginMatchesFilter(const std::string& name, const char* filter) {
    if (filter == nullptr || filter[0] == '\0') return true;
    std::string haystack = name;
    std::string needle = filter;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return haystack.find(needle) != std::string::npos;
}

document::PluginSlot slotFromDescriptor(const engine::PluginDescriptor& d) {
    document::PluginSlot slot;
    slot.name = d.name;
    slot.uidString = d.uidString;
    slot.path = d.path;
    slot.bypass = false;
    return slot;
}

namespace {

// The insert and instrument pickers are the same list with a different filter
// and a different command, so they are the same popup. It is a popup rather
// than a window because choosing a plugin is a step inside adding one — a
// floating browser you have to find, use and then close is three gestures
// where the list is one.
//
// Returns the chosen descriptor, or nullptr while the popup is still open.
const engine::PluginDescriptor* pluginPickerPopup(
    const char* popupId,
    const std::vector<engine::PluginDescriptor>& plugins,
    bool instrumentsOnly,
    char* filter,
    size_t filterSize) {
    const engine::PluginDescriptor* chosen = nullptr;
    if (!ImGui::BeginPopup(popupId)) return nullptr;

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##pluginFilter", "Filter", filter,
                             filterSize);
    ImGui::Separator();

    size_t shown = 0;
    int index = 0;
    for (const auto& d : plugins) {
        if (instrumentsOnly && !d.isInstrument) { ++index; continue; }
        if (!pluginMatchesFilter(d.name, filter)) { ++index; continue; }
        ++shown;
        ImGui::PushID(index);
        if (ImGui::Selectable(d.name.c_str())) {
            chosen = &d;
            ImGui::CloseCurrentPopup();
        }
        if (!d.vendor.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", d.vendor.c_str());
        }
        ImGui::PopID();
        ++index;
    }
    if (shown == 0) {
        ImGui::TextDisabled(instrumentsOnly ? "No instruments found"
                                            : "No plugins found");
    }
    ImGui::EndPopup();
    return chosen;
}

} // namespace

size_t dropIndexAmongRows(float mouseY, const std::vector<float>& rowTops,
                          float listBottom) {
    if (rowTops.empty()) return 0;
    if (mouseY <= rowTops.front()) return 0;
    for (size_t i = 0; i + 1 < rowTops.size(); ++i) {
        if (mouseY < rowTops[i + 1]) return i;
    }
    // Past the last row's top: still the last row, whether or not the mouse
    // has left the list entirely.
    (void)listBottom;
    return rowTops.size() - 1;
}

void drawChannelStrip(document::Edit& edit,
                      editing::UndoStack& undo,
                      TimelineViewState& view,
                      ChannelStripState& strip,
                      int captureChannels,
                      int playbackChannels,
                      bool locked,
                      const std::vector<engine::PluginDescriptor>& plugins,
                      const std::unordered_map<
                          std::string, std::shared_ptr<engine::GainNode>>*
                          meterTaps,
                      LevelMeterOptions* meterOptions) {
    const auto& pal = theme::palette();
    const int sel = view.selectedTrackIndex;
    const int rowCount = static_cast<int>(edit.tracks().size());
    if (sel < 0 || sel >= rowCount) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
        ImGui::TextWrapped("Select a track to see its channel strip.");
        ImGui::PopStyleColor();
        return;
    }

    auto& track = edit.tracksMut()[static_cast<size_t>(sel)];
    const std::string ownerId = track.id;
    // Chosen inside a popup, executed after the strip has finished reading
    // `track` — a command reallocates the track list.
    document::PluginSlot chosenInstrument;
    document::PluginSlot chosenInsert;
    bool haveInstrument = false;
    bool haveInsert = false;

    ImGui::PushID(ownerId.c_str());
    // Rows are uniform and controls are full-width, so the strip reads as one
    // column rather than as a pile of differently-sized boxes. Left-aligned
    // button text because these are list rows, not buttons to press.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(kGutter, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
    const float fullWidth = ImGui::GetContentRegionAvail().x;

    {
        // The track's colour as the header's field, with its name centred on
        // it. The strip belongs to a row in the timeline and nothing else in
        // it says which — a 3 px rule was too quiet to answer that at a
        // glance, and the name had nothing to sit against.
        const ImVec2 headPos = ImGui::GetCursorScreenPos();
        const float headHeight = ImGui::GetFrameHeight() + 6.0f;
        // The same fallback the timeline paints the band with, so an
        // uncoloured track is the same colour in both places.
        const ImVec4 swatch =
            trackColorValue(track.color, defaultTrackColor(sel));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(headPos,
                          ImVec2(headPos.x + fullWidth, headPos.y + headHeight),
                          ImGui::GetColorU32(swatch), 3.0f);

        // Dark text on a light colour and light on a dark one. The picker
        // offers both, so a fixed foreground would be unreadable on half of
        // what the user can choose.
        const ImU32 nameColor =
            ImGui::GetColorU32(theme::readableTextOn(swatch));
        const std::string name = track.name.empty() ? "Untitled" : track.name;
        const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
        dl->PushClipRect(ImVec2(headPos.x + kGutter, headPos.y),
                         ImVec2(headPos.x + fullWidth - kGutter,
                                headPos.y + headHeight),
                         true);
        dl->AddText(ImVec2(headPos.x + (fullWidth - nameSize.x) * 0.5f,
                           headPos.y + (headHeight - nameSize.y) * 0.5f),
                    nameColor, name.c_str());
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(fullWidth, headHeight));
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::BeginDisabled(locked);

    // ─── Input (top) ──────────────────────────────────────────────────────
    // Main and bus-shaped rows have no local source; showing them an input
    // picker would offer a routing that the document would refuse.
    if (!track.isMain) {
        sectionLabel("INPUT");
        const auto options = inputOptions(captureChannels);
        // A dropdown, not a list: an interface with sixteen inputs would push
        // everything below it off the strip, and the current input is the one
        // thing worth showing at a glance.
        //
        // Monitor sits on the same row: it is a property of the input, not a
        // step after it, and a line of its own read as another stage.
        const float monitorWidth =
            ImGui::GetFrameHeight() +
            ImGui::CalcTextSize("Mon").x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(
            std::max(60.0f, fullWidth - monitorWidth -
                                ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::BeginCombo("##hardwareInput",
                              hardwareInputLabel(track.hardwareInput).c_str())) {
            if (options.empty()) {
                ImGui::TextDisabled("No inputs on this device");
            }
            for (const auto& option : options) {
                const bool current =
                    track.hardwareInput.firstChannel == option.firstChannel &&
                    track.hardwareInput.channelCount == option.channelCount;
                ImGui::PushID(option.firstChannel * 2 + option.channelCount);
                if (ImGui::Selectable(hardwareInputLabel(option).c_str(),
                                      current)) {
                    RoutingRequest request;
                    request.kind = RoutingRequest::Kind::SetHardwareInput;
                    request.ownerId = ownerId;
                    request.hardware = option;
                    view.routing.request(std::move(request));
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool monitor = track.inputMonitor;
        if (ImGui::Checkbox("Mon", &monitor)) {
            RoutingRequest request;
            request.kind = RoutingRequest::Kind::SetInputMonitor;
            request.ownerId = ownerId;
            request.enabled = monitor;
            view.routing.request(std::move(request));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Monitor this track's input");
        }
        // An instrument belongs here rather than at the head of the effect
        // chain: it is where this track's audio comes from, exactly like the
        // hardware channel above it. Grouping it with the inserts implied it
        // was something done TO a signal that already existed.
        if (track.instrument.uidString.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
            if (ImGui::Button("Instrument\xe2\x80\xa6",
                              ImVec2(fullWidth, kRowHeight))) {
                strip.pluginFilter[0] = '\0';
                ImGui::OpenPopup("##instrumentPicker");
            }
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button(track.instrument.name.c_str(),
                              ImVec2(fullWidth - kRemoveWidth - kGutter,
                                     kRowHeight))) {
                view.requestPluginEditorSlotId = track.instrument.id;
            }
            ImGui::SameLine(0.0f, kGutter);
            if (ImGui::Button("\xe2\x8c\x84", ImVec2(kRemoveWidth, kRowHeight))) {
                strip.pluginFilter[0] = '\0';
                ImGui::OpenPopup("##instrumentPicker");
            }
        }
        if (const auto* chosen = pluginPickerPopup(
                "##instrumentPicker", plugins, true, strip.pluginFilter,
                sizeof(strip.pluginFilter))) {
            chosenInstrument = slotFromDescriptor(*chosen);
            haveInstrument = true;
        }

    }

    // Above the chain rather than below it: adding is what you do to the
    // list, and a control that follows the thing it acts on has to be hunted
    // for past however many rows are already there. Side by side and centred,
    // because two stacked full-width slabs read as more important than the
    // chain they add to.
    ImGui::Dummy(ImVec2(0.0f, kSectionGap - 4.0f));
    const float addWidth = (fullWidth - kGutter) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ImGui::Button("+ Insert", ImVec2(addWidth, kRowHeight))) {
        strip.pluginFilter[0] = '\0';
        ImGui::OpenPopup("##insertPicker");
    }
    if (const auto* chosen = pluginPickerPopup(
            "##insertPicker", plugins, false, strip.pluginFilter,
            sizeof(strip.pluginFilter))) {
        chosenInsert = slotFromDescriptor(*chosen);
        haveInsert = true;
    }

    // A send is defined by where it goes, so adding one asks that first
    // rather than creating a send to Main and making the user retarget it.
    ImGui::SameLine(0.0f, kGutter);
    if (ImGui::Button("+ Send", ImVec2(addWidth, kRowHeight))) {
        ImGui::OpenPopup("##sendTargetPicker");
    }
    ImGui::PopStyleVar();
    if (ImGui::BeginPopup("##sendTargetPicker")) {
        const auto options =
            routingTargetOptions(edit, ownerId, playbackChannels, true);
        for (const auto& option : options) {
            if (!option.enabled) {
                ImGui::TextDisabled("%s", option.label.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", option.disabledReason.c_str());
                }
                continue;
            }
            if (ImGui::Selectable(option.label.c_str())) {
                document::AuxSend send;
                send.target = option.target;
                RoutingRequest request;
                request.kind = RoutingRequest::Kind::AddSend;
                request.ownerId = ownerId;
                request.send = std::move(send);
                view.routing.request(std::move(request));
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ─── The chain ────────────────────────────────────────────────────────
    // One list. An insert, a send and the fader are all points on the same
    // path, and the order they appear in is the order they happen in — so a
    // send dragged above a compressor hears the signal without it.
    // Whole rows drag; there is no separate handle, because the row IS the
    // thing being moved and a grip column spent width saying so.
    edit.normalizeChainFor_(ownerId);
    const auto& chain = track.chain;
    std::vector<float> chainRowTops;
    chainRowTops.reserve(chain.size());
    const float chainLeft = ImGui::GetCursorScreenPos().x;
    int moveFrom = -1;
    int moveTo = -1;
    std::string removeSlotId;
    std::string removeSendId;
    std::string bypassSlotId;
    bool bypassTo = false;
    document::AuxSend updatedSend;
    bool sendChanged = false;

    engine::GainNode* tapNode = nullptr;
    if (meterTaps != nullptr) {
        const auto it = meterTaps->find(ownerId);
        if (it != meterTaps->end()) tapNode = it->second.get();
    }

    for (size_t row = 0; row < chain.size(); ++row) {
        const auto& slot = chain[row];
        ImGui::PushID(static_cast<int>(row));
        chainRowTops.push_back(ImGui::GetCursorScreenPos().y);
        const bool beingDragged = strip.draggingChainRow == static_cast<int>(row);

        switch (slot.kind) {
            case document::ChainSlot::Kind::Insert: {
                const auto found = std::find_if(
                    track.plugins.begin(), track.plugins.end(),
                    [&](const document::PluginSlot& p) { return p.id == slot.id; });
                if (found == track.plugins.end()) break;
                const ImVec2 namePos = ImGui::GetCursorScreenPos();
                if (found->bypass || beingDragged) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
                }
                const bool clicked = ImGui::Button(
                    found->name.c_str(),
                    ImVec2(fullWidth - kRemoveWidth - kGutter, kRowHeight));
                const ImVec2 nameSize = ImGui::GetItemRectSize();
                if (found->bypass || beingDragged) ImGui::PopStyleColor();
                if (found->bypass) {
                    ImGui::GetWindowDrawList()->AddLine(
                        ImVec2(namePos.x + 6.0f, namePos.y + nameSize.y * 0.5f),
                        ImVec2(namePos.x + nameSize.x - 6.0f,
                               namePos.y + nameSize.y * 0.5f),
                        ImGui::GetColorU32(pal.textSubtle), 1.0f);
                }
                if (ImGui::IsItemActive() &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    strip.draggingChainRow = static_cast<int>(row);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    ImGui::SetTooltip(found->bypass
                        ? "Bypassed — Cmd-click to enable, drag to reorder"
                        : "Click to open, Cmd-click to bypass, drag to reorder");
                }
                if (clicked && !strip.draggingChain()) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeySuper || io.KeyCtrl) {
                        bypassSlotId = found->id;
                        bypassTo = !found->bypass;
                    } else {
                        view.requestPluginEditorSlotId = found->id;
                    }
                }
                ImGui::SameLine(0.0f, kGutter);
                if (ImGui::Button("\xc3\x97", ImVec2(kRemoveWidth, kRowHeight))) {
                    removeSlotId = found->id;
                }
                break;
            }
            case document::ChainSlot::Kind::Send: {
                const auto found = std::find_if(
                    track.sends.begin(), track.sends.end(),
                    [&](const document::AuxSend& x) { return x.id == slot.id; });
                if (found == track.sends.end()) break;
                auto send = *found;
                const std::string targetLabel =
                    routeTargetLabel(edit, send.target, playbackChannels);
                const std::string rowLabel = "\xe2\x86\x92 " + targetLabel;
                const float labelWidth = std::max(
                    60.0f, fullWidth - kSendFaderWidth - kRemoveWidth -
                               kGutter * 2.0f);
                if (ImGui::Button(rowLabel.c_str(),
                                  ImVec2(labelWidth, kRowHeight))) {
                    if (!strip.draggingChain()) ImGui::OpenPopup("##sendRetarget");
                }
                if (ImGui::IsItemActive() &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    strip.draggingChainRow = static_cast<int>(row);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    ImGui::SetTooltip("Send — click to retarget, drag to move "
                                      "it through the chain");
                }
                if (ImGui::BeginPopup("##sendRetarget")) {
                    const auto options = routingTargetOptions(
                        edit, ownerId, playbackChannels, true);
                    for (const auto& option : options) {
                        if (ImGui::Selectable(option.label.c_str(),
                                              send.target == option.target,
                                              option.enabled
                                                  ? ImGuiSelectableFlags_None
                                                  : ImGuiSelectableFlags_Disabled)) {
                            send.target = option.target;
                            updatedSend = send;
                            sendChanged = true;
                        }
                    }
                    ImGui::EndPopup();
                }
                // One row: target, level, remove. A send used to draw as two
                // lines and read as two things.
                ImGui::SameLine(0.0f, kGutter);
                float db = static_cast<float>(gainToDb(send.gain));
                if (sendFader(found->id.c_str(), db, kSendFaderWidth,
                              kRowHeight)) {
                    send.gain = dbToGain(db);
                    updatedSend = send;
                    sendChanged = true;
                }
                ImGui::SameLine(0.0f, kGutter);
                if (ImGui::Button("\xc3\x97", ImVec2(kRemoveWidth, kRowHeight))) {
                    removeSendId = found->id;
                }
                break;
            }
            case document::ChainSlot::Kind::Fader: {
                // The fader's row IS the meter: where it sits is what the row
                // has to say, and the level arriving at it is the level worth
                // watching. The fader control itself is at the bottom of the
                // strip, where it is always reachable however this list is
                // scrolled or arranged.
                LevelMeterOptions fallback;
                LevelMeterOptions& options =
                    meterOptions != nullptr ? *meterOptions : fallback;
                LevelMeterOptions tapOptions = options;
                tapOptions.preFader = true;
                LevelMeterStyle style;
                style.channelWidth = 14.0f;
                style.channelGap = 4.0f;
                style.showTooltip = true;
                const float meterW = levelMeterWidth(style);
                const ImVec2 rowPos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##faderRow",
                                       ImVec2(fullWidth, kMeterRowHeight));
                if (ImGui::IsItemActive() &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    strip.draggingChainRow = static_cast<int>(row);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }
                // A rule through the row: everything above it is pre-fader,
                // everything below is post. The meter sits on the line because
                // the line is where the fader is.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float midY = rowPos.y + kMeterRowHeight * 0.5f;
                const ImU32 rule = ImGui::GetColorU32(pal.border);
                const float halfGap = meterW * 0.5f + 10.0f;
                dl->AddLine(ImVec2(rowPos.x, midY),
                            ImVec2(rowPos.x + fullWidth * 0.5f - halfGap, midY),
                            rule, 1.0f);
                dl->AddLine(ImVec2(rowPos.x + fullWidth * 0.5f + halfGap, midY),
                            ImVec2(rowPos.x + fullWidth, midY), rule, 1.0f);
                drawLevelMeter(
                    tapNode,
                    ImVec2(rowPos.x + (fullWidth - meterW) * 0.5f, rowPos.y),
                    kMeterRowHeight, tapOptions, 2, style,
                    sessionHasFloatHeadroom(edit.bitDepth()));
                break;
            }
        }
        ImGui::PopID();
    }

    const float chainBottom = ImGui::GetCursorScreenPos().y;
    if (strip.draggingChain()) {
        const size_t target = dropIndexAmongRows(ImGui::GetIO().MousePos.y,
                                                 chainRowTops, chainBottom);
        strip.chainDropIndex = static_cast<int>(target);
        if (target < chainRowTops.size()) {
            drawDropLine(chainRowTops[target] - 1.0f, chainLeft,
                         chainLeft + fullWidth);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            moveFrom = strip.draggingChainRow;
            moveTo = strip.chainDropIndex;
            strip.draggingChainRow = -1;
            strip.chainDropIndex = -1;
        }
    }

    // ─── Pan and fader (bottom) ───────────────────────────────────────────
    // Laid out the way a desk channel is: pan across the top, the fader
    // centred below it. The fader's POSITION is a chain row; its VALUE is this
    // control, and burying a control you reach for constantly inside a
    // reorderable list would be the worst of both.
    ImGui::Dummy(ImVec2(0.0f, kSectionGap));
    {
        // Centre an item of `width` in the strip.
        const auto centre = [&](float width) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 std::max(0.0f, (fullWidth - width) * 0.5f));
        };

        // Pan first, and narrow. It is a trim, not the channel's main gesture,
        // and at full width it read as the more important of the two.
        float pan = static_cast<float>(track.pan);
        centre(kPanWidth);
        ImGui::SetNextItemWidth(kPanWidth);
        const bool panDragged =
            ImGui::SliderFloat("##pan", &pan, -1.0f, 1.0f, "");
        const bool panReset = theme::altClickedReset();
        if (panReset) pan = 0.0f;
        if (panDragged || panReset) {
            track.pan = pan;
            edit.notifyChanged();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pan (Option-click for centre)");
        }
        {
            const std::string panText = theme::formatPan(pan);
            ImGui::PushStyleColor(ImGuiCol_Text, pal.textSubtle);
            centre(ImGui::CalcTextSize(panText.c_str()).x);
            ImGui::TextUnformatted(panText.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0.0f, kSectionGap));

        float gainDb = track.gain > 0.0
            ? static_cast<float>(20.0 * std::log10(track.gain)) : -60.0f;
        centre(kFaderWidth);
        const ImVec2 faderPos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 18.0f);
        const bool dragged = ImGui::VSliderFloat(
            "##fader", ImVec2(kFaderWidth, kFaderHeight), &gainDb,
            -60.0f, 6.0f, "");
        ImGui::PopStyleVar();
        // Option-click resets to unity, matching the timeline's gutter fader.
        const bool reset = theme::altClickedReset();
        if (reset) gainDb = 0.0f;
        if (dragged || reset) {
            track.gain = gainDb <= -59.9f ? 0.0
                                          : std::pow(10.0, gainDb / 20.0);
            edit.notifyChanged();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fader (Option-click for 0 dB)");
        }

        // Unity marked on both sides of the travel. A fader whose 0 dB you
        // have to find by watching the number is a fader you cannot set by
        // feel, which is most of what a fader is for.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float unityY =
                faderPos.y + (1.0f - (0.0f - (-60.0f)) / 66.0f) * kFaderHeight;
            const ImU32 tick = ImGui::GetColorU32(pal.textMuted);
            dl->AddLine(ImVec2(faderPos.x - 7.0f, unityY),
                        ImVec2(faderPos.x - 2.0f, unityY), tick, 1.0f);
            dl->AddLine(ImVec2(faderPos.x + kFaderWidth + 2.0f, unityY),
                        ImVec2(faderPos.x + kFaderWidth + 7.0f, unityY),
                        tick, 1.0f);
        }

        char gainText[16];
        std::snprintf(gainText, sizeof(gainText), "%+.1f dB", gainDb);
        centre(ImGui::CalcTextSize(gainText).x);
        ImGui::TextUnformatted(gainText);
    }

    // ─── Output (bottom) ──────────────────────────────────────────────────
    sectionLabel("OUTPUT");
    const std::string route =
        routeTargetLabel(edit, track.mainOutput, playbackChannels);
    ImGui::SetNextItemWidth(fullWidth);
    if (ImGui::BeginCombo("##mainOutput", route.c_str())) {
        const auto options =
            routingTargetOptions(edit, ownerId, playbackChannels, false);
        for (const auto& option : options) {
            if (ImGui::Selectable(option.label.c_str(),
                                  track.mainOutput == option.target,
                                  option.enabled ? ImGuiSelectableFlags_None
                                                 : ImGuiSelectableFlags_Disabled)) {
                RoutingRequest request;
                request.kind = RoutingRequest::Kind::SetMainOutput;
                request.ownerId = ownerId;
                request.route = option.target;
                view.routing.request(std::move(request));
            }
            if (!option.enabled &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", option.disabledReason.c_str());
            }
        }
        ImGui::EndCombo();
    }

    ImGui::EndDisabled();
    ImGui::PopStyleVar(3);
    ImGui::PopID();

    // Mutations run after the loop: removing or reordering in place would
    // invalidate the references the loop is still holding.
    if (!removeSendId.empty()) {
        RoutingRequest request;
        request.kind = RoutingRequest::Kind::RemoveSend;
        request.ownerId = ownerId;
        for (const auto& s : track.sends) {
            if (s.id == removeSendId) { request.send = s; break; }
        }
        view.routing.request(std::move(request));
    } else if (sendChanged) {
        RoutingRequest request;
        request.kind = RoutingRequest::Kind::UpdateSend;
        request.ownerId = ownerId;
        request.send = std::move(updatedSend);
        view.routing.request(std::move(request));
    }
    if (!removeSlotId.empty()) {
        undo.execute(std::make_unique<editing::RemovePluginCommand>(
            ownerId, removeSlotId));
    }
    if (!bypassSlotId.empty()) {
        undo.execute(std::make_unique<editing::SetPluginBypassCommand>(
            bypassSlotId, bypassTo));
    }
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        undo.execute(std::make_unique<editing::MoveChainSlotCommand>(
            ownerId, static_cast<size_t>(moveFrom),
            static_cast<size_t>(moveTo)));
    }
    if (haveInstrument) {
        undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
            ownerId, std::move(chosenInstrument)));
    }
    if (haveInsert) {
        undo.execute(std::make_unique<editing::AddPluginCommand>(
            ownerId, std::move(chosenInsert)));
    }
}

} // namespace dave::gui
