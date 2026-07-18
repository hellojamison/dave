#include "engine/plugins/PluginHost.h"

#include <public.sdk/source/vst/hosting/module.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/vstpresetkeys.h>

#include <cstdio>

namespace dave::engine {

std::vector<PluginDescriptor> PluginHost::scan() {
    descriptors_.clear();

    // Module::getModulePaths() walks the standard VST3 install locations for
    // the platform (e.g. /Library/Audio/Plug-Ins/VST3 on macOS). Loading each
    // module is the slow part — it dlopens the bundle and runs the factory's
    // init code in the plugin's binary.
    auto paths = VST3::Hosting::Module::getModulePaths();
    std::fprintf(stderr, "Dave[plugins]: scanning %zu VST3 path(s)\n", paths.size());

    for (const auto& path : paths) {
        std::string error;
        auto module = VST3::Hosting::Module::create(path, error);
        if (!module) {
            std::fprintf(stderr, "Dave[plugins]: failed to load %s: %s\n",
                         path.c_str(), error.c_str());
            continue;
        }
        auto factory = module->getFactory();
        auto factoryInfo = factory.info();
        for (const auto& classInfo : factory.classInfos()) {
            // Only the audio processor class matters (skip controllers etc.).
            // kVstAudioEffectClass is the VST3 string constant for "Audio Module
            // Class" — the component that processes audio.
            if (classInfo.category() != kVstAudioEffectClass) {
                continue;
            }
            PluginDescriptor d;
            d.name = classInfo.name();
            d.vendor = factoryInfo.vendor();
            d.category = classInfo.category();
            d.subCategories = classInfo.subCategoriesString();
            d.sdkVersion = classInfo.sdkVersion();
            d.path = path;
            d.uidString = classInfo.ID().toString();
            // An instrument is any audio effect whose subcategories contain
            // "Instrument" (or "Synth").
            d.isInstrument =
                d.subCategories.find("Instrument") != std::string::npos ||
                d.subCategories.find("Synth") != std::string::npos;
            descriptors_.push_back(std::move(d));
        }
    }

    std::fprintf(stderr, "Dave[plugins]: found %zu audio plugin(s)\n", descriptors_.size());
    return descriptors_;
}

} // namespace dave::engine
