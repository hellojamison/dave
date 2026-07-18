#include "engine/plugins/PluginInstance.h"

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

namespace dave::engine {

using namespace Steinberg;
using namespace Steinberg::Vst;

// Hidden SDK-owned state. We keep this in a struct so PluginInstance.h doesn't
// have to include the VST3 SDK headers (keeps the rest of the engine SDK-free).
struct PluginInstance::Impl {
    std::shared_ptr<VST3::Hosting::Module> module;
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    IPtr<HostApplication> hostContext;

    // HostProcessData is the SDK helper that allocates per-bus buffers.
    std::unique_ptr<HostProcessData> processData;
    ProcessContext processContext{};
    int maxBlock = 0;
    double sampleRate = 48000.0;
};

PluginInstance::PluginInstance() : p_(std::make_unique<Impl>()) {}
PluginInstance::~PluginInstance() { unload(); }

bool PluginInstance::load(const PluginDescriptor& desc, double sampleRate, int maxBlock) {
    unload();
    name_ = desc.name;
    p_->sampleRate = sampleRate;
    p_->maxBlock = maxBlock;

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

    // 5) Create the edit controller (found via the component's controllerCID).
    //    getControllerClassId writes a raw TUID (int8[16]); convert to VST3::UID.
    TUID controllerCID;
    if (p_->component->getControllerClassId(controllerCID) == kResultTrue) {
        VST3::UID ctrlUid = VST3::UID::fromTUID(controllerCID);
        p_->controller = factory.createInstance<IEditController>(ctrlUid);
        if (p_->controller) {
            p_->controller->initialize(ctx);
        }
    }

    // 6) Prepare the HostProcessData (sets up bus buffers per the component's
    //    declared audio buses). We pass maxBlock; the helper allocates buffers.
    p_->processData = std::make_unique<HostProcessData>();
    if (!p_->processData->prepare(*p_->component, maxBlock, kSample32)) {
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
    int nIn = p_->component->getBusCount(MediaTypes::kAudio, BusDirections::kInput);
    int nOut = p_->component->getBusCount(MediaTypes::kAudio, BusDirections::kOutput);
    std::fprintf(stderr, "Dave[plugins]: loaded '%s' (sr=%.0f block=%d buses in=%d out=%d)\n",
                 name_.c_str(), sampleRate, maxBlock, nIn, nOut);
    return true;
}

void PluginInstance::prepare(double sampleRate, int maxBlock) {
    if (!loaded_) return;
    if (sampleRate == p_->sampleRate && maxBlock == p_->maxBlock) return;

    if (p_->processor) p_->processor->setProcessing(false);
    if (p_->component) p_->component->setActive(false);

    p_->sampleRate = sampleRate;
    p_->maxBlock = maxBlock;
    p_->processData->unprepare();
    p_->processData->prepare(*p_->component, maxBlock, kSample32);

    ProcessSetup setup{kRealtime, kSample32, maxBlock, sampleRate};
    if (p_->processor) p_->processor->setupProcessing(setup);
    if (p_->component) p_->component->setActive(true);
    if (p_->processor) p_->processor->setProcessing(true);
}

void PluginInstance::process(const float* const* in, float* const* out,
                              int numChannels, int numSamples, const TimeInfo& time) {
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
    ctx.state = ProcessContext::kPlaying;
    ctx.tempo = time.bpm;
    pd.processContext = &ctx;

    // Wire our deinterleaved buffers into the HostProcessData's buses.
    // setChannelBuffers wants a non-const Sample32* array. For input we must
    // cast away const (the plugin shouldn't write to inputs, but the SDK type
    // is non-const); for output we hand our writable buffer directly.
    if (pd.inputs != nullptr && numChannels > 0) {
        float* inBufs[8];
        int n = numChannels < 8 ? numChannels : 8;
        for (int c = 0; c < n; ++c) inBufs[c] = const_cast<float*>(in[c]);
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
    if (p_->processData) { p_->processData->unprepare(); p_->processData.reset(); }
    if (p_->controller) { p_->controller = nullptr; }
    if (p_->component) {
        p_->component->terminate();
        p_->component = nullptr;
    }
    p_->hostContext = nullptr;
    p_->module.reset();
    loaded_ = false;
}

} // namespace dave::engine
