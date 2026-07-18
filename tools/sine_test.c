// Standalone miniaudio sine test — bypasses ALL of Dave's code.
// Generates a 440Hz sine directly in the callback and writes it to the device.
// If this is SILENT through Jump Desktop Audio, the problem is miniaudio +
// that device (nothing to do with Dave). If this is AUDIBLE, our Dave code has
// a subtle bug the peak measurement doesn't catch.
//
// Build: see tools/build_sine_test.sh
#include "miniaudio.h"
#include "stdio.h"
#include "math.h"
#include "unistd.h"

static double phase = 0.0;
static const double two_pi = 6.28318530717958647692;

void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    (void)device; (void)input;
    float* out = (float*)output;
    ma_uint32 channels = device->playback.channels;
    double sr = (double)device->sampleRate;
    double delta = two_pi * 440.0 / sr;
    for (ma_uint32 i = 0; i < frameCount; ++i) {
        float s = (float)(0.2 * sin(phase));
        phase += delta;
        if (phase >= two_pi) phase -= two_pi;
        for (ma_uint32 c = 0; c < channels; ++c) {
            out[i * channels + c] = s;
        }
    }
}

int main() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.dataCallback = data_callback;

    ma_device dev;
    if (ma_device_init(NULL, &cfg, &dev) != MA_SUCCESS) {
        printf("init failed\n");
        return 1;
    }
    printf("Opened device: %s @ %u Hz, %u ch\n",
           dev.playback.name ? dev.playback.name : "(default)",
           dev.sampleRate, dev.playback.channels);
    printf("Playing 440Hz sine for 5 seconds... you should hear a tone.\n");
    if (ma_device_start(&dev) != MA_SUCCESS) {
        printf("start failed\n");
        ma_device_uninit(&dev);
        return 1;
    }
    sleep(5);
    ma_device_uninit(&dev);
    printf("Done.\n");
    return 0;
}
