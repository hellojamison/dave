# RB-6 Video NLE — Design Notes

## Honest scope reconsideration

The original plan called for "full video NLE — cut, arrange, transition video
clips." After building RB-5 (single reference video) and seeing what Dave
actually is (an audio-for-picture post DAW, not a Premiere replacement), the
right scope for RB-6 is narrower and more honest:

**Dave's video serves the audio workflow.** You need to *see* the picture to
spot cues, mix to picture, lock markers. A full compositor (transitions,
multi-layer GPU effects) is overkill — that's Resolve/Premiere's job, and a
post engineer running Dave already has one of those open.

## RB-6 scope (what we build)

1. **VideoTrack** in the document — multiple video clips, arranged on a timeline
   lane (like audio tracks but for video). Replaces the single VideoClip.
2. **Video lane in the timeline** — clips render as rectangles with thumbnail
   strips (decoded at low resolution, cached). Same drag/trim/split as audio.
3. **Preview follows the playhead** — whichever video clip is at the current
   position plays in the Video panel. Sequential playback within a clip;
   switch clips when the playhead crosses a boundary.
4. **Export** — composite the video track + mixed audio to a file via the
   LGPL FFmpeg (write each composited frame + the audio mix). Offline render.

## What's deferred (would be over-engineering for Dave's use case)

- GPU transitions (cross-dissolve/wipe/dip) between clips — post users cut
  video in a real NLE; Dave just needs to play it back in sync.
- Per-clip transform effects (scale/pos/rotate/crop) — same reasoning.
- Multi-layer compositing (picture-in-picture, lower-thirds) — not the job.
- A video-FX plugin format — way out of scope.

These can light up later if Dave's identity shifts toward being a real NLE,
but for now they'd burn months for a feature most users would never touch.

## Model changes

```cpp
struct VideoClip {
    // existing fields (path, fps, width, height, durationSeconds) +
    std::string id;
    int64_t timelineStart;   // NEW: position on the timeline (samples @ audio sr)
    int64_t sourceOffset;    // NEW: where in the source to start (seconds*sr)
    int64_t length;          // NEW: how long to play (samples)
    // (was: a single timelineStart=0 clip; now: many, positioned/trimmed)
};

struct VideoTrack {
    std::string id;
    std::string name;
    bool visible = true;
    std::vector<VideoClip> clips;
};
```

The Edit holds a list of VideoTracks (replacing the single VideoClip). For RB-6
we ship one default video track; multiple is trivial once the model exists.

## Compositor (minimal)

For playback, the compositor is: find the video clip whose
[timelineStart, timelineStart+length) contains the playhead. Decode that
clip's frame at (playhead - timelineStart + sourceOffset). Display it. No
layering, no blending — one clip plays at a time. This is exactly what a post
engineer needs: see the picture that goes with the audio at this moment.

## Export (the actually-useful new capability)

Offline render: for each video frame in the timeline's video track, composite
(decoding the right clip per frame), write rawvideo + the mixed audio to
FFmpeg's stdin pipe. Output MP4/MOV. This is the "bounce to picture" workflow
— mix your audio in Dave, attach the video, export a deliverable.

Uses the LGPL FFmpeg subprocess (same as playback). For H.264 *encode* we'd
need either the LGPL built-in encoder (lower quality) or OpenH264 (BSD) —
deferred to when we actually wire export; rawvideo or ProRes is fine for v1.
