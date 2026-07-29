// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/video/VideoDecoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif
#define DAVE_POPEN popen
#define DAVE_PCLOSE pclose
#else
#define DAVE_POPEN _popen
#define DAVE_PCLOSE _pclose
#endif

namespace dave::engine {

// ─── Tool resolution ────────────────────────────────────────────────────────

namespace {
// Default dev location of the bundled LGPL build.
constexpr const char* kBundledDir =
    "third_party/ffmpeg-lgpl/bin";
}

VideoToolPaths resolveVideoTools() {
    VideoToolPaths p;
    p.bundled = false;
    auto exists = [](const std::string& path) {
#if defined(__APPLE__) || defined(__linux__)
        return ::access(path.c_str(), X_OK) == 0;
#else
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#endif
    };

    // 1) Env override (dev convenience).
    const char* env = std::getenv("DAVE_FFMPEG_DIR");
    if (env) {
        p.ffmpeg = std::string(env) + "/ffmpeg";
        p.ffprobe = std::string(env) + "/ffprobe";
        if (exists(p.ffmpeg) && exists(p.ffprobe)) { p.bundled = true; return p; }
    }

    // 2) .app bundle: Contents/Helpers/ (the packaged location — CMake copies
    //    the LGPL build there in the post-build step).
    //    On macOS, the executable is at Contents/MacOS/Dave; Helpers is a
    //    sibling: ../Helpers/ffmpeg relative to the binary. We discover the
    //    binary path via /proc/self/exe (Linux) or _NSGetExecutablePath (Mac).
#if defined(__APPLE__)
    {
        char path[4096];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0) {
            // path = .../Dave.app/Contents/MacOS/Dave
            // Helpers dir = .../Dave.app/Contents/Helpers
            std::string exePath(path);
            auto slash = exePath.find_last_of('/');
            if (slash != std::string::npos) {
                std::string macosDir = exePath.substr(0, slash); // .../MacOS
                slash = macosDir.find_last_of('/');
                if (slash != std::string::npos) {
                    std::string contentsDir = macosDir.substr(0, slash); // .../Contents
                    std::string helpersDir = contentsDir + "/Helpers";
                    p.ffmpeg = helpersDir + "/ffmpeg";
                    p.ffprobe = helpersDir + "/ffprobe";
                    if (exists(p.ffmpeg) && exists(p.ffprobe)) {
                        p.bundled = true;
                        return p;
                    }
                }
            }
        }
    }
#endif

    // 3) Dev location: third_party/ffmpeg-lgpl/bin (relative to cwd).
    p.ffmpeg = std::string(kBundledDir) + "/ffmpeg";
    p.ffprobe = std::string(kBundledDir) + "/ffprobe";
    if (exists(p.ffmpeg) && exists(p.ffprobe)) { p.bundled = true; return p; }

    // 4) System fallback (NOT the license-clean one we ship — dev convenience).
    p.ffmpeg = "ffmpeg";
    p.ffprobe = "ffprobe";
    p.bundled = false;
    return p;
}

// ─── VideoProbe ─────────────────────────────────────────────────────────────

namespace {

// Run a command, capture stdout into a string. Returns false on spawn failure.
bool runCapture(const std::string& cmd, std::string& out) {
    FILE* fp = DAVE_POPEN(cmd.c_str(), "r");
    if (!fp) return false;
    char buf[1024];
    while (size_t n = std::fread(buf, 1, sizeof(buf), fp)) {
        out.append(buf, n);
    }
    DAVE_PCLOSE(fp);
    return true;
}

// Tiny quoted-string escape for shell args (paths with spaces/quotes).
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

bool VideoProbe::probe(const std::string& path, VideoInfo& out) {
    out = VideoInfo{};
    auto tools = resolveVideoTools();
    // Ask ffprobe for the first video stream's codec/width/height/fps + format
    // duration. Use a parse-friendly output format.
    std::string cmd = tools.ffprobe +
        " -v error -select_streams v:0"
        " -show_entries stream=codec_name,width,height,r_frame_rate"
        " -show_entries format=duration"
        " -of default=noprint_wrappers=1 " + shellQuote(path);
    std::string captured;
    if (!runCapture(cmd, captured)) return false;

    // Parse "key=value" lines.
    std::istringstream in(captured);
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "codec_name") out.codec = v;
        else if (k == "width") out.width = std::atoi(v.c_str());
        else if (k == "height") out.height = std::atoi(v.c_str());
        else if (k == "r_frame_rate") {
            // "24000/1001" -> fps.
            auto slash = v.find('/');
            if (slash != std::string::npos) {
                double num = std::atof(v.substr(0, slash).c_str());
                double den = std::atof(v.substr(slash + 1).c_str());
                out.fps = (den != 0.0) ? (num / den) : 0.0;
            }
        } else if (k == "duration") {
            out.durationSeconds = std::atof(v.c_str());
        }
    }
    out.valid = out.width > 0 && out.height > 0 && out.fps > 0.0;
    if (!out.valid) return false;

    // Separate quick probe for an audio stream (for hasAudio / audioCodec).
    std::string cmdA = tools.ffprobe +
        " -v error -select_streams a:0"
        " -show_entries stream=codec_name"
        " -of default=noprint_wrappers=1:nokey=1 " + shellQuote(path);
    std::string capA;
    if (runCapture(cmdA, capA)) {
        // Strip newline.
        auto nl = capA.find_first_of("\r\n");
        if (nl != std::string::npos) capA.erase(nl);
        if (!capA.empty()) {
            out.hasAudio = true;
            out.audioCodec = capA;
        }
    }
    return true;
}

// ─── VideoDecoder ───────────────────────────────────────────────────────────

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const std::string& path, double startSeconds,
                        int width, int height) {
    close();
    auto tools = resolveVideoTools();
    // Seek to startSeconds, scale to width x height, output raw RGBA to stdout.
    // -ss BEFORE -i is faster (input seek). format=rgba ensures 4 bytes/pixel.
    // stderr -> /dev/null: ffmpeg spews "Broken pipe" when we pclose it mid-
    // write (which happens on every seek). That's harmless — ffmpeg gets
    // SIGPIPE and exits — but the noise floods the log and obscures real
    // errors. We suppress it here; real errors come back as readFrame()==false.
    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "%s -hide_banner -loglevel error "
        "-ss %.3f -i %s "
        "-vf \"scale=%d:%d,format=rgba\" "
        "-f rawvideo -pix_fmt rgba "
        "pipe:1 2>/dev/null",
        tools.ffmpeg.c_str(),
        startSeconds,
        shellQuote(path).c_str(),
        width, height);
    FILE* fp = DAVE_POPEN(cmd, "r");
    if (!fp) {
        lastError_ = "popen failed";
        return false;
    }
    pipe_ = fp;
    width_ = width;
    height_ = height;
    frameBytes_ = width * height * 4;
    return true;
}

bool VideoDecoder::readFrame(std::vector<uint8_t>& out) {
    auto* fp = static_cast<FILE*>(pipe_);
    if (!fp) return false;
    out.resize(frameBytes_);
    size_t got = 0;
    while (got < static_cast<size_t>(frameBytes_)) {
        size_t n = std::fread(out.data() + got, 1, frameBytes_ - got, fp);
        if (n == 0) {
            // EOF or error.
            out.clear();
            return false;
        }
        got += n;
    }
    return true;
}

bool VideoDecoder::seekAndRead(const std::string& path, double seconds,
                                int width, int height,
                                std::vector<uint8_t>& out) {
    if (!open(path, seconds, width, height)) return false;
    return readFrame(out);
}

void VideoDecoder::close() {
    if (pipe_) {
        DAVE_PCLOSE(static_cast<FILE*>(pipe_));
        pipe_ = nullptr;
    }
    frameBytes_ = 0;
}

} // namespace dave::engine
