// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dave::engine {

// Probed video metadata. Used at import to populate a VideoClip and to size
// the preview. Populated by VideoProbe::probe().
struct VideoInfo {
    bool valid = false;
    std::string codec;          // e.g. "h264", "dnxhd"
    int width = 0;
    int height = 0;
    double fps = 0.0;           // frames per second (e.g. 23.976)
    double durationSeconds = 0.0;
    bool hasAudio = false;
    std::string audioCodec;
};

// Locate the bundled LGPL ffmpeg/ffprobe binaries. Returns the directory they
// live in (third_party/ffmpeg-lgpl/bin). In dev that's the build output; in a
// packaged app it'd be Contents/Helpers. Falls back to system ffmpeg if the
// bundled one isn't found (dev convenience; the bundled one is the
// license-clean one we ship).
struct VideoToolPaths {
    std::string ffmpeg;
    std::string ffprobe;
    bool bundled; // true if we found the LGPL build, false if system fallback
};
VideoToolPaths resolveVideoTools();

// VideoProbe runs ffprobe to get VideoInfo without decoding.
class VideoProbe {
public:
    // Probe `path` and fill `out`. Returns true on success. Main thread.
    static bool probe(const std::string& path, VideoInfo& out);
};

// VideoDecoder owns an ffmpeg subprocess and pulls RGBA frames from its stdout
// pipe. The model is spawn-per-seek: open() starts ffmpeg at a given position,
// readFrame() pulls the next RGBA frame; seekAndRead() closes + respawns for a
// new position. A frame cache (in DaveApp, not here) amortizes sequential reads.
//
// Threading: all methods run on the UI thread (subprocess control is not
// RT-safe). The RT audio path NEVER touches the decoder — it just reads the
// transport position, and the UI thread maps that to a video frame.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open `path` and start decoding at `startSeconds`. The decoded frames will
    // be `width` x `height` RGBA. Returns false on spawn failure.
    bool open(const std::string& path, double startSeconds, int width, int height);

    // Read the next RGBA frame from the pipe. Returns false at EOF or if the
    // process died. `out` is resized to width*height*4.
    bool readFrame(std::vector<uint8_t>& out);

    // Convenience: close + reopen at a new position, read one frame. Used by
    // random-access (scrubbing / seek). For sequential playback, prefer keeping
    // the process open and calling readFrame() repeatedly.
    bool seekAndRead(const std::string& path, double seconds,
                     int width, int height, std::vector<uint8_t>& out);

    void close();

    bool isOpen() const { return process_ != nullptr; }

private:
    void* process_ = nullptr;       // opaque (FILE* via popen, or a struct)
    void* pipe_ = nullptr;          // stdout read handle
    int frameBytes_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::string lastError_;
};

} // namespace dave::engine
