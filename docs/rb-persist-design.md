# Project Persistence — Design Notes

## Goal

Save/load Dave projects so a session (tracks, clips, plugins, markers, video)
survives a restart. Nothing persists today — this is the biggest functional gap.

## Format: .dave bundle (a directory)

Per the architecture doc — a `.dave` "file" is actually a directory bundle:

```
mysession.dave/
├── project.json        — THE document (text, diffable, VCS-friendly)
├── assets/             — copied media, content-addressed (SHA-256)
│   └── ab12cd...wav
└── video/              — the reference movie (copied in)
    └── mymovie.mov
```

**Why self-contained (copy media in):** matches Pro Tools session convention,
survives source-file moves/renames, portable across machines. Tradeoff: disk
space (large movies get duplicated). Acceptable for v1; a "reference external
files" mode is a later option.

**Why JSON not binary:** diffable, git-friendly, recoverable by hand if
corrupted. The architecture doc committed to this; we honor it.

## Schema (project.json, versioned)

```jsonc
{
  "format": "dave.doc/v1",
  "sampleRate": 48000,
  "tracks": [
    { "id": "track_1", "name": "Track 1", "gain": 1.0, "pan": 0.0,
      "clips": [
        { "id": "clip_1", "asset": "ab12cd...", "timelineStart": 0,
          "sourceOffset": 0, "length": 48000, "gain": 1.0,
          "fadeIn": 64, "fadeOut": 64 }
      ],
      "plugins": [
        { "id": "plugin_1", "name": "Melodyne", "uidString": "...",
          "path": "/Library/Audio/Plug-Ins/VST3/Melodyne.vst3", "bypass": false }
      ]
    }
  ],
  "assets": [
    { "id": "ab12cd...", "path": "assets/ab12cd....wav", "sampleRate": 48000,
      "channels": 2, "lengthSamples": 48000 }
  ],
  "markerTracks": [
    { "id": "mtrack_1", "name": "Markers", "visible": true,
      "markers": [
        { "id": "marker_1", "name": "Door slam", "kind": "cue",
          "posMode": "sample", "position": 96000, "length": 0,
          "color": "", "metadata": "" }
      ] }
  ],
  "videoClip": {
    "path": "video/mymovie.mov", "name": "mymovie.mov", "codec": "h264",
    "timelineStart": 0, "fps": 23.976, "width": 1920, "height": 1080,
    "durationSeconds": 120.0
  }
}
```

Notes:
- **Plugin paths are absolute** (VST3 install locations are system-standard).
  This is how every DAW does it; plugins aren't copied into the bundle.
- **Asset paths are relative** to the bundle (they're copied in).
- **Plugin state (parameter values) is NOT saved in v1** — that's a follow-up
  (needs the VST3 getState/setState chunk, which we haven't wired). Plugins
  reload at their default state for now.

## Atomic save

Write `project.json.tmp` → fsync → atomic rename. Same pattern as the
architecture doc specified. Prevents corruption if Dave crashes mid-save.

## Module

`src/document/ProjectFile.{h,cpp}` — serialize(Edit) -> json, deserialize(json)
-> Edit, plus saveBundle(path, Edit) / loadBundle(path) -> Edit. The bundle
creates `assets/` + `video/` dirs and copies media on save; load re-links
relative paths.

## UI

File menu: New / Open... / Save / Save As... + a dirty indicator in the title
bar (`*` when unsaved changes). Dirty bit set by every Command; cleared on
save.
