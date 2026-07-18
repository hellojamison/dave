// Standalone plugin host test: loads a VST3, feeds it a sine, prints the
// output peak. Proves the entire VST3 hosting chain works without involving
// Dave's graph/UI. Run after building Dave:
//   cmake --build build && ./build/plugin_test <path-to-plugin.vst3>
#include "engine/plugins/PluginHost.h"
#include "engine/plugins/PluginInstance.h"

#include <cstdio>
#include <cmath>
#include <cstring>

int main(int argc, char** argv) {
    using namespace dave::engine;

    // 1) Scan for plugins (or use a path from argv[1]).
    PluginHost host;
    auto descs = host.scan();
    PluginDescriptor target;
    bool found = false;
    if (argc > 1) {
        // argv[1] is a plugin name substring to match (e.g. "ResoCalm").
        for (const auto& d : descs) {
            if (d.name.find(argv[1]) != std::string::npos) {
                target = d; found = true; break;
            }
        }
    } else if (!descs.empty()) {
        target = descs.front(); found = true;
    }
    if (!found) {
        std::fprintf(stderr, "test: no plugin found (scanned %zu)\n", descs.size());
        return 1;
    }
    std::fprintf(stderr, "test: targeting '%s' by %s\n", target.name.c_str(), target.vendor.c_str());

    // 2) Load + prepare the instance.
    PluginInstance inst;
    const double sr = 48000.0;
    const int block = 256;
    if (!inst.load(target, sr, block)) {
        std::fprintf(stderr, "test: load failed: %s\n", inst.lastError().c_str());
        return 1;
    }

    // 3) Generate a 440Hz sine as input, run it through the plugin, measure
    //    the output peak over several blocks.
    const int channels = 2;
    float inBuf[channels][block];
    float outBuf[channels][block];
    float* inPtrs[channels] = {inBuf[0], inBuf[1]};
    float* outPtrs[channels] = {outBuf[0], outBuf[1]};

    double phase = 0.0;
    const double twoPi = 6.28318530717958647692;
    const double delta = twoPi * 440.0 / sr;
    float maxOutPeak = 0.0f;
    TimeInfo time{};
    time.sampleRate = sr;
    time.bpm = 120.0;
    time.isPlaying = true;

    for (int b = 0; b < 40; ++b) { // ~200ms of audio
        for (int i = 0; i < block; ++i) {
            float s = (float)(0.2 * std::sin(phase));
            phase += delta;
            if (phase >= twoPi) phase -= twoPi;
            inBuf[0][i] = inBuf[1][i] = s;
        }
        time.samplePos = b * block;
        std::memset(outBuf, 0, sizeof(outBuf));
        inst.process(inPtrs, outPtrs, channels, block, time);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < block; ++i)
                maxOutPeak = std::max(maxOutPeak, std::fabs(outBuf[c][i]));
    }

    std::fprintf(stderr, "test: input sine peak=0.2, plugin OUTPUT peak=%.4f after 40 blocks\n",
                 maxOutPeak);
    if (maxOutPeak > 0.0001f) {
        std::fprintf(stderr, "test: PASS — plugin produced nonzero audio output.\n");
        return 0;
    } else {
        std::fprintf(stderr, "test: FAIL — plugin output is silent.\n");
        return 2;
    }
}
