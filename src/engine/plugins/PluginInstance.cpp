// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/plugins/PluginInstance.h"

#include <public.sdk/source/common/memorystream.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/processdata.h>
#include <public.sdk/source/vst/utility/uid.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/base/funknownimpl.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>

namespace dave::engine {

using namespace Steinberg;
using namespace Steinberg::Vst;

class DaveComponentHandler final : public IComponentHandler {
public:
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultTrue; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override {
        return kResultTrue;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultTrue; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if ((flags & kLatencyChanged) != 0) {
            latencyChanged.store(true, std::memory_order_release);
        }
        return kResultTrue;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** object) override {
        QUERY_INTERFACE(iid, object, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(iid, object, IComponentHandler::iid, IComponentHandler)
        *object = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    std::atomic<bool> latencyChanged{false};
};

// Hidden SDK-owned state. We keep this in a struct so PluginInstance.h doesn't
// have to include the VST3 SDK headers (keeps the rest of the engine SDK-free).
struct PluginInstance::Impl {
    std::shared_ptr<VST3::Hosting::Module> module;
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    IPtr<HostApplication> hostContext;
    DaveComponentHandler componentHandler;

    // HostProcessData is the SDK helper that allocates per-bus buffers.
    std::unique_ptr<HostProcessData> processData;
    ProcessContext processContext{};
    int maxBlock = 0;
    double sampleRate = 48000.0;

    // Pre-allocated event lists. 512 is chosen to cover the worst realistic
    // block — a dense cluster plus a 128-note all-notes-off flush after a seek
    // — so addEvent never has to grow anything on the RT thread. Events past
    // capacity are dropped, which costs a note, not a glitch.
    static constexpr int kEventCapacity = 512;
    EventList inputEvents{kEventCapacity};
    EventList outputEvents{kEventCapacity};

    // Silent input for generators. An instrument is driven by events, not
    // audio, so its caller passes in == nullptr — but a synth may still declare
    // an audio input bus (a vocoder side-chain, a "audio in" mode), and
    // HostProcessData::prepare(…, 0, …) leaves those channel pointers null.
    // Handing the plugin a real block of zeroes is cheaper than auditing every
    // synth for whether it reads a bus it declared.
    static constexpr int kMaxChannels = 8;
    std::vector<float> silenceStorage;
    float* silenceChannels[kMaxChannels]{};

    void resizeSilence(int blockSize) {
        silenceStorage.assign(static_cast<size_t>(blockSize) * kMaxChannels, 0.0f);
        for (int c = 0; c < kMaxChannels; ++c) {
            silenceChannels[c] = silenceStorage.data() + static_cast<size_t>(c) * blockSize;
        }
    }
};

PluginInstance::PluginInstance() : p_(std::make_unique<Impl>()) {}
PluginInstance::~PluginInstance() { unload(); }

bool PluginInstance::load(const PluginDescriptor& desc, double sampleRate, int maxBlock) {
    unload();
    name_ = desc.name;
    p_->sampleRate = sampleRate;
    p_->maxBlock = maxBlock;
    p_->resizeSilence(maxBlock);

    // 1) Load the .vst3 bundle as a hosting Module (dlopens the binary).
    std::string err;
    p_->module = VST3::Hosting::Module::create(desc.path, err);
    if (!p_->module) {
        lastError_ = "Module::create failed: " + err;
        return false;
    }

    // 2) Get the factory + parse the descriptor's UID. UID::fromString returns
    //    an Optional<UID>; we bail if the string is malformed.
    auto factory = p_->module->getFactory();
    auto uidOpt = VST3::UID::fromString(desc.uidString);
    if (!uidOpt) {
        lastError_ = "bad UID string: " + desc.uidString;
        return false;
    }
    VST3::UID uid = *uidOpt;

    // 3) Create the component (the audio processor side).
    p_->component = factory.createInstance<IComponent>(uid);
    if (!p_->component) {
        lastError_ = "failed to create IComponent";
        return false;
    }

    // 4) Initialize the component with a host application context. Some plugins
    //    need this to find host services.
    p_->hostContext = owned(new HostApplication());
    FUnknown* ctx = p_->hostContext;
    if (p_->component->initialize(ctx) != kResultOk) {
        lastError_ = "IComponent::initialize failed";
        p_->component = nullptr;
        return false;
    }

    // 5) Create the edit controller. Two paths (per Steinberg's host guidance):
    //    a) getControllerClassId returns a CID → instantiate it from the factory.
    //    b) Fallback: query the COMPONENT directly for IEditController. Many
    //       plugins (single-binary, or "simple" ones) implement both the
    //       processor and the controller on the same object, and don't report
    //       a separate controllerCID. Without this fallback, Melodyne (and
    //       others) load fine but have no editor.
    TUID controllerCID;
    if (p_->component->getControllerClassId(controllerCID) == kResultTrue) {
        VST3::UID ctrlUid = VST3::UID::fromTUID(controllerCID);
        p_->controller = factory.createInstance<IEditController>(ctrlUid);
        if (p_->controller) {
            p_->controller->initialize(ctx);
            // Per VST3 spec, connect the controller to the component so parameter
            // changes propagate. (Best-effort; not all plugins need it.)
            FUnknownPtr<IConnectionPoint> componentCP(p_->component);
            FUnknownPtr<IConnectionPoint> controllerCP(p_->controller);
            if (componentCP && controllerCP) {
                componentCP->connect(controllerCP);
            }
        }
    }
    if (!p_->controller) {
        // Fallback: the component may itself implement IEditController.
        p_->controller = FUnknownPtr<IEditController>(p_->component);
        // Note: do NOT call initialize on a component-obtained controller — it
        // shares the component's already-initialized state.
    }
    if (p_->controller) {
        p_->controller->setComponentHandler(&p_->componentHandler);
    }

    // 6) Activate only the main audio buses (bus 0 in/out). Plugins with extra
    //    buses (sidechain, aux) need those deactivated, or their unwired
    //    buffers can crash process(). This is the standard host behavior.
    int nIn = p_->component->getBusCount(MediaTypes::kAudio, BusDirections::kInput);
    for (int i = 0; i < nIn; ++i) {
        p_->component->activateBus(MediaTypes::kAudio, BusDirections::kInput, i,
                                    (i == 0) ? 1 : 0);
    }
    int nOut = p_->component->getBusCount(MediaTypes::kAudio, BusDirections::kOutput);
    for (int i = 0; i < nOut; ++i) {
        p_->component->activateBus(MediaTypes::kAudio, BusDirections::kOutput, i,
                                    (i == 0) ? 1 : 0);
    }

    // 6b) Event buses. A VST3 instrument's note input is a bus like any other
    //     and starts DEACTIVATED — a synth whose event bus was never activated
    //     loads cleanly, reports no error, and stays silent forever, which is
    //     the single most confusing failure mode in VST3 hosting. Activate the
    //     main event bus in each direction for every plugin: effects simply
    //     report zero event buses and this loop does nothing.
    const int nEventIn = p_->component->getBusCount(MediaTypes::kEvent,
                                                    BusDirections::kInput);
    for (int i = 0; i < nEventIn; ++i) {
        p_->component->activateBus(MediaTypes::kEvent, BusDirections::kInput, i,
                                    (i == 0) ? 1 : 0);
    }
    acceptsMidi_ = nEventIn > 0;
    const int nEventOut = p_->component->getBusCount(MediaTypes::kEvent,
                                                     BusDirections::kOutput);
    for (int i = 0; i < nEventOut; ++i) {
        p_->component->activateBus(MediaTypes::kEvent, BusDirections::kOutput, i,
                                    (i == 0) ? 1 : 0);
    }

    // 7) Prepare the HostProcessData. We pass bufferSamples=0 so the helper
    //    sets up the bus/channel arrays BUT does NOT allocate its own sample
    //    buffers (channelBufferOwner stays false). That lets our setChannelBuffers
    //    calls in process() redirect the channel pointers to OUR buffers each
    //    block. If we passed maxBlock here, the helper would own the buffers and
    //    silently ignore setChannelBuffers (returning false) — which is exactly
    //    the bug that produced silent plugin output.
    p_->processData = std::make_unique<HostProcessData>();
    if (!p_->processData->prepare(*p_->component, 0, kSample32)) {
        lastError_ = "HostProcessData::prepare failed";
        unload();
        return false;
    }

    // 7) Get the audio processor interface.
    p_->processor = FUnknownPtr<IAudioProcessor>(p_->component);
    if (!p_->processor) {
        lastError_ = "component has no IAudioProcessor";
        unload();
        return false;
    }

    // 8) Configure + activate processing.
    ProcessSetup setup{kRealtime, kSample32, maxBlock, sampleRate};
    if (p_->processor->setupProcessing(setup) != kResultOk) {
        lastError_ = "setupProcessing failed";
        unload();
        return false;
    }
    if (p_->component->setActive(true) != kResultOk) {
        lastError_ = "setActive(true) failed";
        unload();
        return false;
    }
    if (p_->processor->setProcessing(true) != kResultOk) {
        lastError_ = "setProcessing(true) failed";
        unload();
        return false;
    }

    loaded_ = true;
    int logNIn = p_->component->getBusCount(MediaTypes::kAudio, BusDirections::kInput);
    int logNOut = p_->component->getBusCount(MediaTypes::kAudio, BusDirections::kOutput);
    std::fprintf(stderr, "Dave[plugins]: loaded '%s' (sr=%.0f block=%d buses in=%d out=%d)\n",
                 name_.c_str(), sampleRate, maxBlock, logNIn, logNOut);
    return true;
}

void PluginInstance::prepare(double sampleRate, int maxBlock) {
    if (!loaded_) return;
    if (sampleRate == p_->sampleRate && maxBlock == p_->maxBlock) return;

    if (p_->processor) p_->processor->setProcessing(false);
    if (p_->component) p_->component->setActive(false);

    p_->sampleRate = sampleRate;
    p_->maxBlock = maxBlock;
    p_->resizeSilence(maxBlock);
    p_->processData->unprepare();
    p_->processData->prepare(*p_->component, 0, kSample32); // 0 = don't own buffers

    ProcessSetup setup{kRealtime, kSample32, maxBlock, sampleRate};
    if (p_->processor) p_->processor->setupProcessing(setup);
    if (p_->component) p_->component->setActive(true);
    if (p_->processor) p_->processor->setProcessing(true);
}

uint32_t PluginInstance::latencySamples() const {
    return loaded_ && p_->processor ? p_->processor->getLatencySamples() : 0;
}

bool PluginInstance::consumeLatencyChange() {
    if (!loaded_ || !p_->component || !p_->processor ||
        !p_->componentHandler.latencyChanged.exchange(
            false, std::memory_order_acq_rel)) {
        return false;
    }
    // VST3 requires a deactivate/reactivate cycle before querying the new
    // latency. This runs on the UI thread; the live immutable graph continues
    // using the old instance until the caller publishes its replacement.
    p_->processor->setProcessing(false);
    p_->component->setActive(false);
    ProcessSetup setup{kRealtime, kSample32, p_->maxBlock, p_->sampleRate};
    p_->processor->setupProcessing(setup);
    p_->component->setActive(true);
    p_->processor->setProcessing(true);
    return true;
}

bool PluginInstance::latencyChangePending() const {
    return loaded_ &&
        p_->componentHandler.latencyChanged.load(std::memory_order_acquire);
}

void PluginInstance::process(const float* const* in, float* const* out,
                              int numChannels, int numSamples,
                              const MidiEvent* events, int numEvents,
                              const TimeInfo& time) {
    if (!loaded_ || !p_->processor || !p_->processData) return;

    // --- RT thread ---
    auto& pd = *p_->processData;
    pd.numSamples = numSamples;

    // Fill the ProcessContext.
    auto& ctx = p_->processContext;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.sampleRate = p_->sampleRate;
    ctx.projectTimeSamples = time.samplePos;
    ctx.continousTimeSamples = time.samplePos; // Steinberg's spelling (no 'u')
    // The musical position and tempo have to be flagged valid or a synth's
    // tempo-synced LFOs and delays read zero and free-run. kPlaying is a flag,
    // not the whole state word: asserting it unconditionally told every plugin
    // the transport was rolling even while stopped, so anything gated on
    // transport state (a synced arpeggiator, a gate) ran during silence.
    ctx.state = ProcessContext::kProjectTimeMusicValid | ProcessContext::kTempoValid;
    if (time.isPlaying) ctx.state |= ProcessContext::kPlaying;
    ctx.projectTimeMusic = time.ppqPosition;
    ctx.tempo = time.bpm;
    pd.processContext = &ctx;

    // Translate our block-relative MIDI into the SDK's Event union. The lists
    // are pre-allocated members; clear() just resets a fill count.
    p_->inputEvents.clear();
    p_->outputEvents.clear();
    for (int i = 0; i < numEvents; ++i) {
        const MidiEvent& m = events[i];
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = m.sampleOffset;
        e.ppqPosition = time.ppqPosition;
        e.flags = Event::kIsLive;
        if (m.type == MidiEventType::NoteOn) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = m.channel;
            e.noteOn.pitch = m.pitch;
            e.noteOn.tuning = 0.0f;
            e.noteOn.velocity = static_cast<float>(m.velocity) / 127.0f;
            e.noteOn.length = 0;
            // -1 means "no note id": we address notes by pitch+channel, which
            // is what a plain SMF gives us. Note ids belong with per-note
            // expression, which arrives with the piano roll.
            e.noteOn.noteId = -1;
        } else {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = m.channel;
            e.noteOff.pitch = m.pitch;
            e.noteOff.velocity = static_cast<float>(m.velocity) / 127.0f;
            e.noteOff.noteId = -1;
            e.noteOff.tuning = 0.0f;
        }
        // Returns kResultFalse when full; dropping the overflow is the RT-safe
        // choice and cannot happen below a 512-event block.
        p_->inputEvents.addEvent(e);
    }
    pd.inputEvents = &p_->inputEvents;
    pd.outputEvents = &p_->outputEvents;

    // Wire our deinterleaved buffers into the HostProcessData's buses.
    // setChannelBuffers wants a non-const Sample32* array. For input we must
    // cast away const (the plugin shouldn't write to inputs, but the SDK type
    // is non-const); for output we hand our writable buffer directly.
    if (pd.inputs != nullptr && numChannels > 0) {
        float* inBufs[Impl::kMaxChannels];
        int n = numChannels < Impl::kMaxChannels ? numChannels : Impl::kMaxChannels;
        for (int c = 0; c < n; ++c) {
            inBufs[c] = (in != nullptr) ? const_cast<float*>(in[c])
                                        : p_->silenceChannels[c];
        }
        pd.setChannelBuffers(BusDirections::kInput, 0, inBufs, n);
    }
    if (pd.outputs != nullptr && numChannels > 0) {
        float* outBufs[8];
        int n = numChannels < 8 ? numChannels : 8;
        for (int c = 0; c < n; ++c) outBufs[c] = out[c];
        pd.setChannelBuffers(BusDirections::kOutput, 0, outBufs, n);
    }

    p_->processor->process(pd);
}

void PluginInstance::unload() {
    if (p_->processor) { p_->processor->setProcessing(false); p_->processor = nullptr; }
    if (p_->component) { p_->component->setActive(false); }
    if (p_->processData) {
        // Drop our event-list pointers before the ProcessData goes away, so a
        // stale unload/reload can't hand a plugin a dangling list.
        p_->processData->inputEvents = nullptr;
        p_->processData->outputEvents = nullptr;
        p_->processData->unprepare();
        p_->processData.reset();
    }
    if (p_->controller) {
        p_->controller->setComponentHandler(nullptr);
        p_->controller = nullptr;
    }
    if (p_->component) {
        p_->component->terminate();
        p_->component = nullptr;
    }
    p_->hostContext = nullptr;
    p_->module.reset();
    loaded_ = false;
    acceptsMidi_ = false;
}

void* PluginInstance::editControllerAsUnknown() const {
    if (!loaded_ || !p_->controller) return nullptr;
    Steinberg::FUnknown* fu = static_cast<Steinberg::Vst::IEditController*>(p_->controller.get());
    return fu;
}

// ─── State save/restore ─────────────────────────────────────────────────────
// Minimal base64 encoder (no external dep). Public domain implementation.
namespace {
const char b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += b64Table[(n >> 18) & 63];
        out += b64Table[(n >> 12) & 63];
        out += (i + 1 < len) ? b64Table[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? b64Table[n & 63] : '=';
    }
    return out;
}
int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    int val = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int d = b64Val(c);
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
        }
    }
    return out;
}
} // namespace

std::string PluginInstance::getStateBase64() const {
    if (!loaded_ || !p_->component) return "";
    auto* memStream = new Steinberg::MemoryStream;
    Steinberg::IBStream* stream = static_cast<Steinberg::IBStream*>(memStream);
    p_->component->setActive(false);
    auto res = p_->component->getState(stream);
    p_->component->setActive(true);
    if (res != Steinberg::kResultOk) { memStream->release(); return ""; }
    auto* data = memStream->getData();
    auto size = memStream->getSize();
    std::string result;
    if (data && size > 0) {
        result = base64Encode(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(size));
    }
    memStream->release();
    return result;
}

bool PluginInstance::setStateBase64(const std::string& b64) {
    if (!loaded_ || !p_->component || b64.empty()) return false;
    auto bytes = base64Decode(b64);
    if (bytes.empty()) return false;
    auto* memStream = new Steinberg::MemoryStream(
        bytes.data(), static_cast<Steinberg::TSize>(bytes.size()));
    Steinberg::IBStream* stream = memStream;
    auto res = p_->component->setState(stream);
    if (p_->controller && res == Steinberg::kResultOk) {
        auto* memStream2 = new Steinberg::MemoryStream(
            bytes.data(), static_cast<Steinberg::TSize>(bytes.size()));
        Steinberg::IBStream* stream2 = memStream2;
        p_->controller->setComponentState(stream2);
        memStream2->release();
    }
    memStream->release();
    return res == Steinberg::kResultOk;
}

} // namespace dave::engine
