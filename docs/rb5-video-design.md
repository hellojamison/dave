# RB-5 Video Playback — Design Notes

## Goal

Play a video file locked to Dave's audio transport (audio-for-picture / post
playback). A VideoPreview panel shows the frame at the current playhead; the
audio clock is master. RB-5 is playback only — no editing/transitions (that's
RB-6 NLE).

## The OverSync-proven model

Decouple Dave from FFmpeg entirely by **spawning ffmpeg as a subprocess** and
reading raw RGBA frames from its stdout pipe. Dave never links libav*.

```
Dave (C++) ──spawn──► ffmpeg -ss <seek> -i movie.mp4 \
                              -vf scale=W:H,format=rgba \
                              -f rawvideo -pix_fmt rgba pipe:1
                      │
                      ▼ stdout (RGBA bytes)
                  frame buffer
```

Why subprocess (not linking the LGPL libs):
- **Cleanest licensing** — separate process over a pipe is "mere aggregation",
  not a derivative work. Zero combined-work question. Preserves dual licensing
  even more clearly than dynamic linking would.
- **Free sandboxing** — a crashing ffmpeg can't take Dave down.
- **Matches OverSync** — Jamison's already proven this works frame-accurately.
- **No link/install_name complexity** — we just need the ffmpeg binary on disk.

Cost: process startup per seek (~50-200ms). Mitigated by an LRU frame cache +
seeking only when the playhead jumps >1s or direction reverses.

## Components

1. **VideoProbe** — runs `ffprobe` to get codec/duration/fps/resolution without
   decoding. Used at import (to populate the VideoClip) and to size the preview.
2. **VideoDecoder** — owns an ffmpeg subprocess; pulls RGBA frames from its pipe.
   API: `open(path, w, h)`, `seekAndRead(seconds) -> RGBA bytes`, `close()`.
   Spawns a fresh ffmpeg per seek (simple + robust; cache amortizes this).
3. **VideoClip** (document) — path, duration, fps, width, height, timelineStart.
4. **VideoPreview panel** (GUI) — uploads the latest RGBA to a GL texture, draws
   it via `ImGui::Image`. A/V sync: each frame, the transport samplePos tells us
   the desired video time; we ask the decoder for that frame (or reuse cache).
5. **A/V sync** — audio is master. From the playhead's `samplePos / 48000`, we
   compute the target video time, request that frame (frame cache hits avoid
   respawning ffmpeg), and display it. Drift corrected only at frame boundaries.

## Frame cache

Small LRU keyed by `(clipPath, frameIndex, width)`. Default 24 frames (~1s at
24fps). Prevents respawning ffmpeg every block while playing sequentially.
For RB-5, sequential playback hits the cache after the first frame per second.

## Licensing note (the whole point of this approach)

The bundled ffmpeg is **LGPL-only** (built via scripts/build-ffmpeg-lgpl-macos.sh
— no --enable-gpl, no libx264/x265). It enables h264/hevc/dnxhd *decoders*
(LGPL built-ins) — only the *encoders* (libx264/x265) are GPL, and we don't use
them. Combined with the subprocess model, Dave's own code never links GPL code
and the commercial licensing option is fully preserved.
