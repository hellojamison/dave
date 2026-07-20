#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dave::engine {

// AsyncVideoDecoder runs ffmpeg on a background thread, decoding frames into a
// ring buffer of RGBA data. The UI thread requests frames by position; the
// background thread fills them. This solves TWO problems at once:
//
// 1. Smooth playback — the pipe read no longer blocks the UI thread (which was
//    the root cause of stutter/stripes during playback).
// 2. Thumbnails — the same background decoder can extract representative frames
//    for timeline thumbnails without stalling.
//
// Threading:
// - requestFrame() / getLatestFrame() / cancel(): UI thread.
// - The internal worker thread runs ffmpeg and reads RGBA from the pipe.
// - No locks held during pipe read (the UI thread reads the latest frame via
//   a double-buffered swap).

struct VideoFrame {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    double timeSeconds = 0.0;   // position in the source
    int64_t frameIndex = -1;
    bool valid = false;
};

class AsyncVideoDecoder {
public:
    AsyncVideoDecoder();
    ~AsyncVideoDecoder();

    // Request a frame at `timeSeconds` from `path` at resolution w×h.
    // If the worker is busy, the request queues (or replaces a pending one).
    // Results appear in getLatestFrame().
    void requestFrame(const std::string& path, double timeSeconds,
                      int width, int height, double fps);

    // Get the most recent decoded frame (UI thread). Returns false if none.
    bool getLatestFrame(VideoFrame& out);

    // True if a request is pending (worker hasn't finished).
    bool isBusy() const { return busy_.load(std::memory_order_acquire); }

    // Stop the worker + clear state.
    void shutdown();

private:
    void workerLoop();

    struct Request {
        std::string path;
        double timeSeconds = 0.0;
        int width = 0;
        int height = 0;
        double fps = 24.0;
    };

    std::atomic<bool> running_{false};
    std::atomic<bool> busy_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    VideoFrame latestFrame_;
    std::thread thread_;
};

} // namespace dave::engine
