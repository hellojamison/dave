# RB-4 Markers — Design Notes

## Goal

A first-class marker system that reflects Dave's post-production identity.
Markers are not an afterthought — they're a core editing surface, modeled on
the workflow Jamison built into OverMarker.

## Scope for RB-4 (this phase)

- **Marker model** with the four position modes from the architecture doc, but
  **sample-absolute as the only fully-wired mode for RB-4**. SMPTE display,
  musical (tempo-map-bound), and clip-anchored are modeled but their conversion
  math lands later (SMPTE needs a frame-rate field; musical needs the tempo map
  that arrives with MIDI). Storing the mode now means we don't migrate later.
- **Marker tracks** (categories): the Edit holds a list of marker tracks, each
  with its own markers. UI shows/hides per track. RB-4 ships one default
  "Markers" track; the UI lets you add more (e.g. "Cues", "Scenes").
- **Kinds**: point + region (region = start+length). Kinds (cue/section/loop/
  punch/cd/custom) drive color + behavior.
- **Behaviors wired in RB-4:** loop region (transport loops it), punch region
  (punch in/out — stub for now since recording isn't in yet), snap-to-marker.
- **Behaviors deferred:** cue → fires MIDI/OSC (needs OSC/MIDI out plumbing),
  CD → export split (needs export pipeline).
- **Import/export:** Reaper region/marker CSV (the de-facto interchange). This
  is the most useful one to have first — overlaps with OverMarker's world.

## Marker model

```cpp
enum class MarkerKind { Cue, Section, Loop, Punch, CD, Custom };
enum class MarkerPosMode { Sample, Smpte, Musical, ClipAnchored };

struct Marker {
    std::string id;
    std::string name;
    MarkerKind kind = MarkerKind::Cue;
    MarkerPosMode posMode = MarkerPosMode::Sample;
    int64_t position = 0;     // in posMode units (samples for Sample)
    int64_t length = 0;       // 0 = point marker; >0 = region
    std::string color;        // hex "#rrggbb", default per kind
    std::string clipId;       // for ClipAnchored: the parent clip
    std::string metadata;     // free-form (cue#, scene, take, etc.)
};

struct MarkerTrack {
    std::string id;
    std::string name;
    bool visible = true;
    std::vector<Marker> markers;
};
```

Position resolution: at render/transport time, resolve each marker to a sample
position. For RB-4 (Sample-only wired) it's identity. The resolver is the seam
where SMPTE/tempo-map/clip-anchored math plugs in.

## UI

A dedicated **marker lane** at the top of the timeline (above the track rows,
below the ruler). Markers render as flags/triangles (point) or bars (region),
colored by kind. Interactions:
- **Double-click empty marker lane** → add a point marker at that position.
- **Drag a marker** → move it (MoveMarkerCommand on release).
- **Drag a region's edges** → resize.
- **Right-click a marker** → context menu (rename, change kind, set color,
  delete, make region).
- **Click a marker** → seek transport to it (handy for navigation).

Snap-to-marker toggle in the transport bar; when on, clip drag/seek snap to
nearby markers.

## Why this shape

- **Serializable**: markers live in the Edit, get saved with the project, are
  undoable like everything else.
- **Category-friendly**: multiple marker tracks = the OverMarker mental model
  of "cues vs scenes vs sync points" as separate layers.
- **Extensible**: the metadata field + kind enum cover cue-list/show-control
  use cases (the takeoneaudio.com lineage) without schema changes.
- **Mode-forward**: storing posMode now means SMPTE/musical/clip-anchored can
  light up later without a migration.
