// SPDX-License-Identifier: GPL-3.0-or-later
//
// The channel strip's send list.
//
// Sends sum, so their order changes nothing audible — which is exactly why the
// document has to preserve the order the user arranged rather than deriving
// one. The parts worth testing are the drop-index arithmetic, the move itself,
// and undo, which has to restore the index a send came FROM rather than move it
// back by the distance it travelled: once other rows have shifted around it
// those are different answers.
#include "ImGuiTestRig.h"

#include "editing/Commands.h"
#include "engine/GraphBuilder.h"
#include "gui/ChannelStrip.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace dave;

namespace {

std::vector<std::string> sendOrder(const document::Edit& edit,
                                   const std::string& ownerId) {
    std::vector<std::string> ids;
    if (const auto* t = edit.track(ownerId)) {
        for (const auto& s : t->sends) ids.push_back(s.id);
    }
    return ids;
}

// Three sends on one track, each pointing somewhere valid.
struct SendFixture {
    document::Edit edit;
    editing::UndoStack undo{edit};
    std::string owner;
    std::vector<std::string> ids;

    SendFixture() {
        owner = edit.addTrack("Source");
        const std::string a = edit.addBus("A");
        const std::string b = edit.addBus("B");
        for (const std::string& target : {a, b, std::string(document::kMainBusId)}) {
            document::AuxSend send;
            send.target = document::RouteTarget::bus(target);
            const std::string id = edit.addSend(owner, send);
            REQUIRE_FALSE(id.empty());
            ids.push_back(id);
        }
    }
};

} // namespace

// ─── Drop-index arithmetic ─────────────────────────────────────────────────

// ─── The document move ─────────────────────────────────────────────────────

// ─── Undo ──────────────────────────────────────────────────────────────────

// ─── The gesture, through the real widget ──────────────────────────────────

namespace {

struct StripRig : dave::testing::ImGuiRig {
    void tick(float x, float y, bool down) {
        frame(x, y, down, [&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(260.0f, 900.0f));
            ImGui::Begin("strip");
            gui::drawChannelStrip(edit, undo, view, strip, 4, 2, false);
            ImGui::End();
        });
    }

    void dragFrom(ImVec2 from, ImVec2 to) {
        tick(from.x, from.y, false);
        tick(from.x, from.y, true);
        tick((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f, true);
        tick(to.x, to.y, true);
        tick(to.x, to.y, false);
    }

    gui::ChannelStripState strip;
};

} // namespace

TEST_CASE("a drop index follows the rows' real heights", "[channelstrip]") {
    // Rows 22 px, 50 px and 22 px tall: a uniform assumption would put the
    // boundary between rows 1 and 2 in the middle of row 1.
    const std::vector<float> tops{100.0f, 122.0f, 172.0f};
    constexpr float bottom = 194.0f;

    CHECK(gui::dropIndexAmongRows(90.0f, tops, bottom) == 0);
    CHECK(gui::dropIndexAmongRows(110.0f, tops, bottom) == 0);
    CHECK(gui::dropIndexAmongRows(130.0f, tops, bottom) == 1);
    CHECK(gui::dropIndexAmongRows(170.0f, tops, bottom) == 1);
    CHECK(gui::dropIndexAmongRows(180.0f, tops, bottom) == 2);
    // Past the end is still the last row, not off it.
    CHECK(gui::dropIndexAmongRows(9000.0f, tops, bottom) == 2);
    CHECK(gui::dropIndexAmongRows(50.0f, {}, bottom) == 0);
}

TEST_CASE("the fader's unity tick sits where ImGui draws the 0 dB grab",
          "[channelstrip]") {
    // ImGui insets a slider's grab travel by grab_padding (2px) + grab_sz/2 at
    // each end, so the tick has to use that same geometry. The reference
    // numbers are computed straight from SliderBehaviorT for a 200px fader with
    // an 18px grab: usable travel [11, 189].
    constexpr float h = 200.0f;
    constexpr float grab = 18.0f;
    auto at = [&](float db) {
        return gui::verticalSliderGrabCenterY(0.0f, h, grab, db, -60.0f, 6.0f);
    };
    // The endpoints land at the grab-centre insets, never the raw frame edges
    // (which is what a full-height mapping would have used).
    CHECK(std::abs(at(6.0f) - 11.0f) < 0.01f);     // +6 dB pinned to the top
    CHECK(std::abs(at(-60.0f) - 189.0f) < 0.01f);  // -60 dB pinned to the bottom
    // 0 dB: grabT = 1 - 60/66, so 11 + 178 * (6/66) = 27.18 — a full grab-half
    // below the 18.18 the old full-height formula produced.
    CHECK(std::abs(at(0.0f) - 27.1818f) < 0.01f);
    // Never higher than the naive mapping: the inset only pushes the tick down.
    CHECK(at(0.0f) > 0.0f + (1.0f - 60.0f / 66.0f) * h);
}

TEST_CASE("the picker filter is case-insensitive and matches anywhere",
          "[channelstrip]") {
    CHECK(gui::pluginMatchesFilter("Pro-Q 3", ""));
    CHECK(gui::pluginMatchesFilter("Pro-Q 3", nullptr));
    CHECK(gui::pluginMatchesFilter("Pro-Q 3", "pro"));
    CHECK(gui::pluginMatchesFilter("Pro-Q 3", "PRO"));
    // Anywhere, not just the start — plugin names lead with a vendor prefix
    // more often than not.
    CHECK(gui::pluginMatchesFilter("FabFilter Pro-Q 3", "q 3"));
    CHECK_FALSE(gui::pluginMatchesFilter("Pro-Q 3", "reverb"));
}

TEST_CASE("a chosen descriptor becomes a playable slot", "[channelstrip]") {
    engine::PluginDescriptor d;
    d.name = "Pro-Q 3";
    d.uidString = "0123456789abcdef0123456789abcdef";
    d.path = "/Library/Audio/Plug-Ins/VST3/Pro-Q 3.vst3";
    d.vendor = "FabFilter";

    const auto slot = gui::slotFromDescriptor(d);
    CHECK(slot.name == d.name);
    CHECK(slot.uidString == d.uidString);
    CHECK(slot.path == d.path);
    // Adding a plugin bypassed would look like it failed to load.
    CHECK_FALSE(slot.bypass);
    // The id is minted by the document when the slot lands, not here.
    CHECK(slot.id.empty());
}

// ─── The chain-position meter ──────────────────────────────────────────────
//
// The meter is a row in the insert chain, so where it sits IS where it reads.
// Two things have to hold for that to be true rather than decorative: the
// document has to survive a round trip, and the graph has to put a real tap
// node at that point.

TEST_CASE("a tap passes audio through untouched", "[channelstrip][meter]") {
    // The pan law is 0.707 a side at centre, so a unity-gain GainNode dropped
    // into the chain would quietly cost it 3 dB. A tap has to bypass the law
    // entirely, or moving the meter would change the mix.
    engine::GainNode tap;
    tap.setMeterTapOnly(true);
    tap.prepare(48000.0, 8);

    std::array<float, 8> leftIn{};
    std::array<float, 8> rightIn{};
    std::array<float, 8> leftOut{};
    std::array<float, 8> rightOut{};
    leftIn.fill(0.5f);
    rightIn.fill(-0.25f);
    float* inPointers[2] = {leftIn.data(), rightIn.data()};
    float* outPointers[2] = {leftOut.data(), rightOut.data()};

    engine::AudioBus inputBus{inPointers, 2, 8};
    engine::TimeInfo time;
    time.samplePos = 0;
    engine::NodeProcessContext context;
    context.numSamples = 8;
    context.time = &time;
    context.inputs = &inputBus;
    context.numInputs = 1;
    context.output = engine::AudioBus{outPointers, 2, 8};
    tap.process(context);

    for (size_t i = 0; i < 8; ++i) {
        CHECK(leftOut[i] == 0.5f);
        CHECK(rightOut[i] == -0.25f);
    }
    // And it measured what went through it.
    CHECK(tap.meter(0, true).peak >= 0.5f);
    CHECK(tap.meter(1, true).peak >= 0.25f);
}

TEST_CASE("a normal gain node still applies the pan law",
          "[channelstrip][meter]") {
    // The guard for the test above: without it, "passes through untouched"
    // would also pass if the law had simply been deleted.
    engine::GainNode gain;
    gain.setGain(1.0);
    gain.prepare(48000.0, 8);

    std::array<float, 8> leftIn{};
    std::array<float, 8> rightIn{};
    std::array<float, 8> leftOut{};
    std::array<float, 8> rightOut{};
    leftIn.fill(1.0f);
    rightIn.fill(1.0f);
    float* inPointers[2] = {leftIn.data(), rightIn.data()};
    float* outPointers[2] = {leftOut.data(), rightOut.data()};

    engine::AudioBus inputBus{inPointers, 2, 8};
    engine::TimeInfo time;
    time.samplePos = 0;
    engine::NodeProcessContext context;
    context.numSamples = 8;
    context.time = &time;
    context.inputs = &inputBus;
    context.numInputs = 1;
    context.output = engine::AudioBus{outPointers, 2, 8};
    gain.process(context);

    CHECK(leftOut[7] < 0.72f);   // ~0.707, not 1.0
    CHECK(leftOut[7] > 0.70f);
}

TEST_CASE("the graph taps the chain where the chain says", "[channelstrip][meter]") {
    // A meter row the graph never taps would read silence while claiming to
    // read the chain — worse than no meter, because it looks like an answer.
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    for (int i = 0; i < 2; ++i) {
        document::PluginSlot slot;
        slot.name = "Fx" + std::to_string(i);
        slot.uidString = "uid" + std::to_string(i);
        REQUIRE_FALSE(edit.addPlugin(t, slot).empty());
    }
    edit.normalizeChainFor_(t);

    engine::GraphBuilder builder;
    auto graph = builder.build(edit, 48000.0, 2);
    REQUIRE(graph != nullptr);

    const auto& taps = builder.meterTaps();
    REQUIRE(taps.count(t) == 1);
    REQUIRE(taps.at(t) != nullptr);
    // It has to be a tap, not an ordinary gain stage — an ordinary one in the
    // chain would attenuate.
    CHECK(taps.at(t)->meterTapOnly());
    // Main is a row like any other, so it gets one too.
    CHECK(taps.count(std::string(document::kMainBusId)) == 1);
}

TEST_CASE("choosing an instrument sets it rather than adding an insert",
          "[channelstrip]") {
    // The instrument moved up beside the hardware input, because it is where
    // the track's audio comes from rather than something done to a signal that
    // already exists. What must not move with it is which command it issues.
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Keys");

    engine::PluginDescriptor d;
    d.name = "Surge";
    d.uidString = "abcdefabcdefabcdefabcdefabcdefab";
    d.path = "/Library/Audio/Plug-Ins/VST3/Surge.vst3";
    d.isInstrument = true;

    undo.execute(std::make_unique<editing::SetMidiInstrumentCommand>(
        t, gui::slotFromDescriptor(d)));

    CHECK(edit.track(t)->instrument.name == "Surge");
    CHECK(edit.track(t)->instrument.uidString == d.uidString);
    // Not in the effect chain — a synth in the inserts would be fed silence.
    CHECK(edit.track(t)->plugins.empty());

    undo.undo();
    CHECK(edit.track(t)->instrument.uidString.empty());
}

TEST_CASE("Cmd-clicking an insert bypasses it, plain click opens it",
          "[channelstrip]") {
    // The bypass checkbox is gone — it spent a column on a state the row can
    // simply show. What replaced it has to distinguish the two gestures on the
    // same target, which is the part worth pinning.
    StripRig rig;
    const std::string owner = rig.edit.addTrack("Audio");
    document::PluginSlot slot;
    slot.name = "Compressor";
    slot.uidString = "uidcomp";
    const std::string slotId = rig.edit.addPlugin(owner, slot);
    REQUIRE_FALSE(slotId.empty());
    rig.view.selectedTrackIndex = 0;

    // Probe for the insert's button: a plain click on it asks for its editor.
    float insertY = -1.0f;
    for (float y = 20.0f; y < 860.0f; y += 2.0f) {
        rig.view.requestPluginEditorSlotId.clear();
        rig.tick(90.0f, y, false);
        rig.tick(90.0f, y, true);
        rig.tick(90.0f, y, false);
        if (rig.view.requestPluginEditorSlotId == slotId) { insertY = y; break; }
    }
    REQUIRE(insertY > 0.0f);
    // A plain click must not have toggled anything.
    CHECK_FALSE(rig.edit.track(owner)->plugins.front().bypass);

    // The same click with the modifier held bypasses instead.
    rig.view.requestPluginEditorSlotId.clear();
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Super, true);
    rig.tick(90.0f, insertY, false);
    rig.tick(90.0f, insertY, true);
    rig.tick(90.0f, insertY, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Super, false);

    CHECK(rig.edit.track(owner)->plugins.front().bypass);
    // And it opened nothing — the two gestures must not both fire.
    CHECK(rig.view.requestPluginEditorSlotId.empty());

    // Bypass is a mix decision made while comparing, which is exactly when you
    // want to take it back. It used to write straight to the document.
    rig.undo.undo();
    CHECK_FALSE(rig.edit.track(owner)->plugins.front().bypass);
    rig.undo.redo();
    CHECK(rig.edit.track(owner)->plugins.front().bypass);

    // It toggles rather than latching.
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Super, true);
    rig.tick(90.0f, insertY, false);
    rig.tick(90.0f, insertY, true);
    rig.tick(90.0f, insertY, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Super, false);
    CHECK_FALSE(rig.edit.track(owner)->plugins.front().bypass);
}

TEST_CASE("a bypassed insert keeps its place in the chain", "[channelstrip]") {
    // Bypassing must not renumber the chain: the meter and any sends around
    // that insert would slide to a different point in the signal.
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        document::PluginSlot slot;
        slot.name = "Fx" + std::to_string(i);
        slot.uidString = "uid" + std::to_string(i);
        ids.push_back(edit.addPlugin(t, slot));
    }
    edit.normalizeChainFor_(t);
    const auto before = edit.track(t)->chain;

    REQUIRE(edit.setPluginBypass(ids[0], true));
    edit.normalizeChainFor_(t);
    const auto& after = edit.track(t)->chain;
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        CHECK(after[i].kind == before[i].kind);
        CHECK(after[i].id == before[i].id);
    }
}

TEST_CASE("bypass resolves a slot by id alone, instruments included",
          "[channelstrip]") {
    // The callers that bypass a slot have its id without the track it belongs
    // to, and an instrument is as bypassable as an insert.
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string a = edit.addTrack("One");
    const std::string b = edit.addTrack("Two");

    document::PluginSlot insert;
    insert.name = "EQ";
    insert.uidString = "uideq";
    const std::string insertId = edit.addPlugin(b, insert);
    REQUIRE_FALSE(insertId.empty());

    document::PluginSlot instrument;
    instrument.name = "Surge";
    instrument.uidString = "uidsurge";
    instrument.id = "slot_instrument";
    REQUIRE(edit.setMidiInstrument(a, instrument));

    REQUIRE(edit.pluginSlot(insertId) != nullptr);
    REQUIRE(edit.pluginSlot("slot_instrument") != nullptr);
    CHECK(edit.pluginSlot("slot_nonexistent") == nullptr);

    undo.execute(std::make_unique<editing::SetPluginBypassCommand>(
        "slot_instrument", true));
    CHECK(edit.track(a)->instrument.bypass);
    undo.undo();
    CHECK_FALSE(edit.track(a)->instrument.bypass);

    // Setting a slot to the state it already holds is not an edit, so undoing
    // it must not flip anything.
    undo.execute(std::make_unique<editing::SetPluginBypassCommand>(
        insertId, false));
    undo.undo();
    CHECK_FALSE(edit.track(b)->plugins.front().bypass);
}

// ─── One chain ─────────────────────────────────────────────────────────────
//
// Inserts, sends, the meter and the fader are points on one path. Two parallel
// lists could not say "send, then insert, then send", and a send tapped before
// a compressor is a different send from one tapped after it.

namespace {

std::vector<std::string> chainSummary(const document::Edit& edit,
                                      const std::string& trackId) {
    std::vector<std::string> out;
    const auto* t = edit.track(trackId);
    if (t == nullptr) return out;
    for (const auto& slot : t->chain) {
        switch (slot.kind) {
            case document::ChainSlot::Kind::Insert:
                out.push_back("insert:" + slot.id); break;
            case document::ChainSlot::Kind::Send:
                out.push_back("send:" + slot.id); break;
            case document::ChainSlot::Kind::Fader:
                out.push_back("fader"); break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("a track has a chain from the moment it exists", "[channelstrip]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    // A meter and a fader at minimum, so nothing downstream has to handle an
    // empty chain.
    CHECK(chainSummary(edit, t) == std::vector<std::string>{"fader"});
    CHECK(chainSummary(edit, std::string(document::kMainBusId)) ==
          std::vector<std::string>{"fader"});
}

TEST_CASE("new inserts land before the fader, new sends after",
          "[channelstrip]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    const std::string fx = edit.addPlugin(t, document::PluginSlot{});
    document::AuxSend send;
    send.target = document::RouteTarget::bus();
    const std::string s = edit.addSend(t, send);
    REQUIRE_FALSE(fx.empty());
    REQUIRE_FALSE(s.empty());

    // A send defaults after the fader: one that suddenly tapped ahead of it
    // would ignore the fader, which is not what "add a send" should mean.
    CHECK(chainSummary(edit, t) ==
          std::vector<std::string>{"insert:" + fx, "fader", "send:" + s});
}

TEST_CASE("a pre-fader send starts ahead of the fader", "[channelstrip]") {
    // AuxSend::tap is a placement hint and nothing more — but it has to be
    // honoured, or every project that used it reopens sounding different.
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    document::AuxSend pre;
    pre.target = document::RouteTarget::bus();
    pre.tap = document::SendTap::PreFader;
    const std::string s = edit.addSend(t, pre);
    REQUIRE_FALSE(s.empty());

    CHECK(chainSummary(edit, t) ==
          std::vector<std::string>{"send:" + s, "fader"});
}

TEST_CASE("a send can be moved ahead of an insert and back", "[channelstrip]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    const std::string fx = edit.addPlugin(t, document::PluginSlot{});
    document::AuxSend send;
    send.target = document::RouteTarget::bus();
    const std::string s = edit.addSend(t, send);

    const auto before = chainSummary(edit, t);
    REQUIRE(before ==
            std::vector<std::string>{"insert:" + fx, "fader",
                                     "send:" + s});

    // Drag the send to the head of the chain: it now hears the source without
    // the insert, which two parallel lists could not have expressed.
    undo.execute(std::make_unique<editing::MoveChainSlotCommand>(t, 2, 0));
    CHECK(chainSummary(edit, t) ==
          std::vector<std::string>{"send:" + s, "insert:" + fx,
                                   "fader"});

    undo.undo();
    CHECK(chainSummary(edit, t) == before);
}

TEST_CASE("the chain drops entries whose payload has gone", "[channelstrip]") {
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    const std::string fx = edit.addPlugin(t, document::PluginSlot{});
    REQUIRE(chainSummary(edit, t).size() == 2);

    REQUIRE(edit.removePlugin(t, fx));
    // No entry naming a plugin that no longer exists, and the meter and fader
    // survive the removal.
    CHECK(chainSummary(edit, t) == std::vector<std::string>{"fader"});
}


TEST_CASE("the fader's row is the only one the chain adds by itself",
          "[channelstrip]") {
    // The meter is not a separate entry any more: the fader's position is the
    // one thing that row has to say, and drawing it as a meter says where the
    // level is read at the same time.
    document::Edit edit;
    const std::string t = edit.addTrack("Audio");
    const auto& chain = edit.track(t)->chain;
    REQUIRE(chain.size() == 1);
    CHECK(chain[0].kind == document::ChainSlot::Kind::Fader);
    CHECK(chain[0].id.empty());
}

TEST_CASE("the fader row survives being dragged and undone", "[channelstrip]") {
    document::Edit edit;
    editing::UndoStack undo{edit};
    const std::string t = edit.addTrack("Audio");
    std::vector<std::string> fx;
    for (int i = 0; i < 2; ++i) {
        document::PluginSlot slot;
        slot.name = "Fx" + std::to_string(i);
        fx.push_back(edit.addPlugin(t, slot));
    }
    REQUIRE(chainSummary(edit, t) ==
            std::vector<std::string>{"insert:" + fx[0], "insert:" + fx[1],
                                     "fader"});

    // Fader to the head: both inserts are now post-fader, which is a real and
    // reachable arrangement rather than a broken one.
    undo.execute(std::make_unique<editing::MoveChainSlotCommand>(t, 2, 0));
    CHECK(chainSummary(edit, t) ==
          std::vector<std::string>{"fader", "insert:" + fx[0],
                                   "insert:" + fx[1]});

    // A new insert lands at the end of the insert run, not back above the
    // fader the user deliberately moved.
    document::PluginSlot third;
    third.name = "Fx2";
    const std::string fx2 = edit.addPlugin(t, third);
    CHECK(chainSummary(edit, t) ==
          std::vector<std::string>{"fader", "insert:" + fx[0],
                                   "insert:" + fx[1], "insert:" + fx2});
}

// ─── Meter placement and output pinning ─────────────────────────────────────

namespace {
// A strip rig that passes meter options through, so the below-fader placement
// and the bottom-pinned output can be exercised at the real widget.
struct MeteredStripRig : dave::testing::ImGuiRig {
    gui::ChannelStripState strip;
    gui::LevelMeterOptions meterOptions;

    void render(float windowHeight) {
        frame(-100.0f, -100.0f, false, [&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(260.0f, windowHeight));
            ImGui::Begin("strip");
            gui::drawChannelStrip(edit, undo, view, strip, 4, 2, false, {},
                                  nullptr, &meterOptions);
            ImGui::End();
        });
    }
};

bool chainHasFader(const dave::document::Edit& edit, const std::string& id) {
    const auto* t = edit.track(id);
    if (t == nullptr) return false;
    for (const auto& slot : t->chain) {
        if (slot.kind == dave::document::ChainSlot::Kind::Fader) return true;
    }
    return false;
}
} // namespace

TEST_CASE("the strip renders with the meter in either place",
          "[channelstrip][meter]") {
    // A pure layout change must not touch the document: no undo entries, and
    // the chain keeps its single fader slot whichever way the meter is shown.
    // Both placements are exercised at two heights so the output-pinning
    // spacer is tested where there is spare room and where there is not.
    MeteredStripRig rig;
    const std::string track = rig.edit.addTrack("Dialog");
    rig.view.selectedTrackIndex =
        static_cast<int>(rig.edit.tracks().size()) - 1;
    REQUIRE(chainHasFader(rig.edit, track));

    for (bool below : {false, true, false}) {
        rig.meterOptions.belowFader = below;
        rig.render(900.0f);   // spare room below -> output pushed to the base
        rig.render(240.0f);   // cramped -> the spacer must clamp, not overflow
        CHECK(rig.undo.undoDepth() == 0);
        CHECK(chainHasFader(rig.edit, track));
    }
}

TEST_CASE("the below-fader meter option persists through the menu path",
          "[channelstrip][meter]") {
    // The option lives on LevelMeterOptions and is toggled from the meter's own
    // menu; here we set it directly and confirm the strip honours it without
    // resetting it — the failure that would make the toggle feel dead.
    MeteredStripRig rig;
    const std::string track = rig.edit.addTrack("Music");
    rig.view.selectedTrackIndex =
        static_cast<int>(rig.edit.tracks().size()) - 1;

    rig.meterOptions.belowFader = true;
    rig.render(700.0f);
    CHECK(rig.meterOptions.belowFader);   // the strip must not clear it
}

TEST_CASE("dragging the meter down moves it below the fader, up restores it",
          "[channelstrip][meter]") {
    // The meter toggles its placement on a vertical drag. The flip has to reach
    // the real options, not the pre-fader copy the strip hands the meter — the
    // bug that made the drag look dead.
    MeteredStripRig rig;
    rig.edit.addTrack("Vox");
    rig.view.selectedTrackIndex =
        static_cast<int>(rig.edit.tracks().size()) - 1;
    rig.meterOptions.belowFader = false;

    auto body = [&] {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(260.0f, 900.0f));
        ImGui::Begin("strip");
        gui::drawChannelStrip(rig.edit, rig.undo, rig.view, rig.strip, 4, 2,
                              false, {}, nullptr, &rig.meterOptions);
        ImGui::End();
    };

    // The in-chain meter sits at the fader row; sweep to find it, then drag it
    // down past the threshold.
    const float x = 130.0f;
    bool movedDown = false;
    for (float y = 30.0f; y < 500.0f && !movedDown; y += 4.0f) {
        rig.meterOptions.belowFader = false;
        rig.frame(x, y, false, body);
        rig.frame(x, y, true, body);
        rig.frame(x, y + 15.0f, true, body);
        rig.frame(x, y + 40.0f, true, body);
        rig.frame(x, y + 40.0f, false, body);
        movedDown = rig.meterOptions.belowFader;
    }
    CHECK(movedDown);
    // A placement change is layout only — it never touches the document.
    CHECK(rig.undo.undoDepth() == 0);
}

