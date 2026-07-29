// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/video/AsyncVideoDecoder.h"
#include "engine/video/VideoDecoder.h"

#include <cstdio>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#define DAVE_POPEN popen
#define DAVE_PCLOSE pclose
#else
#define DAVE_POPEN _popen
#define DAVE_PCLOSE _pclose
#endif

namespace dave::engine {

AsyncVideoDecoder::AsyncVideoDecoder() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AsyncVideoDecoder::workerLoop, this);
}

AsyncVideoDecoder::~AsyncVideoDecoder() {
    shutdown();
}

void AsyncVideoDecoder::shutdown() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void AsyncVideoDecoder::requestFrame(const std::string& path, double timeSeconds,
                                      int width, int height, double fps) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        // Replace any pending request (we only care about the latest).
        queue_.clear();
        queue_.push_back({path, timeSeconds, width, height, fps});
    }
    busy_.store(true, std::memory_order_release);
    cv_.notify_one();
}

bool AsyncVideoDecoder::getLatestFrame(VideoFrame& out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!latestFrame_.valid) return false;
    out = latestFrame_;
    return true;
}

void AsyncVideoDecoder::workerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        Request req;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [&] {
                return !queue_.empty() || !running_.load(std::memory_order_acquire);
            });
            if (!running_.load(std::memory_order_acquire)) break;
            if (queue_.empty()) continue;
            req = queue_.front();
            queue_.pop_front();
        }

        // Spawn ffmpeg to decode one frame at the requested position.
        auto tools = resolveVideoTools();
        char cmd[2048];
        std::snprintf(cmd, sizeof(cmd),
            "%s -hide_banner -loglevel error "
            "-ss %.3f -i \"%s\" "
            "-vf \"scale=%d:%d,format=rgba\" "
            "-f rawvideo -pix_fmt rgba "
            "-frames:v 1 "
            "pipe:1 2>/dev/null",
            tools.ffmpeg.c_str(),
            req.timeSeconds,
            req.path.c_str(),
            req.width, req.height);

        FILE* fp = DAVE_POPEN(cmd, "r");
        if (!fp) {
            busy_.store(false, std::memory_order_release);
            continue;
        }

        int frameBytes = req.width * req.height * 4;
        std::vector<uint8_t> buf(frameBytes);
        size_t got = 0;
        while (got < static_cast<size_t>(frameBytes)) {
            size_t n = std::fread(buf.data() + got, 1, frameBytes - got, fp);
            if (n == 0) break;
            got += n;
        }
        DAVE_PCLOSE(fp);

        if (got == static_cast<size_t>(frameBytes)) {
            std::lock_guard<std::mutex> lock(mtx_);
            latestFrame_.rgba = std::move(buf);
            latestFrame_.width = req.width;
            latestFrame_.height = req.height;
            latestFrame_.timeSeconds = req.timeSeconds;
            latestFrame_.frameIndex = static_cast<int64_t>(req.timeSeconds * req.fps);
            latestFrame_.valid = true;
        }

        busy_.store(false, std::memory_order_release);
    }
}

} // namespace dave::engine
