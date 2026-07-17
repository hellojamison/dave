# RB-2 — Document Model & Timeline Design

## The central decision: document vs. graph

RB-1 built the engine graph (`engine::Graph`/`CompiledGraph`) as the thing the
UI edited directly. That was fine for a 3-node demo, but it does not scale to a
DAW: users think in **tracks and clips**, not nodes and edges. Two-source-of-
truth drift is the classic DAW architecture bug.

RB-2 introduces a **document model** as the single source of truth:

```
   Document (Edit)              ── what the user edits, what's saved/undone
     │
     │ derive (UI thread, on every edit)
     ▼
   engine::Graph  ──compile──►  CompiledGraph  ──►  AudioEngine (RT)
```

- **Edit** = tracks, clips, assets, tempo map. Serialized to `project.json`.
  Every mutation goes through a **Command** (undoable).
- The **engine graph is derived** from the Edit by a `GraphBuilder`: for each
  audio clip → an `AudioClipNode` at the clip's position; all clips on a track
  sum into a track node; tracks sum into master. (Routing-grid editing of the
  derived graph is a later phase — RB-2 uses a fixed derive rule.)
- When the Edit changes, we re-derive the graph, recompile, and publish to the
  RT thread (the RB-1 atomic-swap path). This is exactly the pattern the
  architecture doc already described.

## Document model

```cpp
namespace dave::document {

struct AssetId { std::string sha256; };        // content-addressed

struct AudioAsset {
    AssetId id;
    std::string path;                          // resolved file on disk
    int sampleRate;
    int channels;
    int64_t lengthSamples;
};

struct AudioClip {
    std::string id;                            // stable across edits
    AssetId asset;
    int64_t timelineStart;                     // where on the timeline
    int64_t sourceOffset;                      // where in the source file
    int64_t length;                            // how long (may be < asset length)
    double gain = 1.0;
    int64_t fade_in_samples = 0, fade_out_samples = 0;
};

struct Track {
    std::string id;
    std::string name;
    double gain = 1.0;
    double pan = 0.0;
    std::vector<AudioClip> clips;
};

class Edit {
public:
    // Adds an asset (dedupes by content hash). Returns the id.
    AssetId importAsset(const std::string& filePath);
    // Adds a clip to a track (assigns a stable id). Returns the clip id.
    std::string addClip(const std::string& trackId, AudioClip clip);
    std::string addTrack(std::string name);

    // Mutators all go through Command for undo — see Command.h.
    const std::vector<Track>& tracks() const { return tracks_; }
    const AudioAsset* asset(const AssetId&) const;

private:
    std::vector<Track> tracks_;
    std::unordered_map<AssetId, AudioAsset> assets_;
};

}
```

Stable IDs matter: when the user drags a clip, we want to *update* that clip's
`timelineStart`, not recreate it — so undo works, and the engine can diff.

## Command pattern

Every user-visible mutation is a `Command` with `perform()`/`undo()`. The
`UndoStack` holds executed commands. Each command, on perform/undo, mutates the
Edit and then triggers a graph re-derive + recompile + publish.

```cpp
struct Command {
    virtual ~Command() = default;
    virtual void perform(Edit&) = 0;
    virtual void undo(Edit&) = 0;
    virtual std::string name() const = 0;
};

class UndoStack {
    std::vector<std::unique_ptr<Command>> done_;
    // ...
    void execute(std::unique_ptr<Command>, Edit&);  // perform + push
    void undo(Edit&);
    void redo(Edit&);
};
```

RB-2 commands: `AddTrack`, `RemoveTrack`, `AddClip`, `MoveClip`, `TrimClip`,
`RemoveClip`. (Cut/split/fade come with editing ops.)

## Edit → Graph derivation (`GraphBuilder`)

For each track: one `SummingNode` (N inputs = N clips). Each clip → one
`AudioClipNode` wired to the track's sum. All track sums → a master
`SummingNode` → a `GainNode` (master). This is the RB-1 topology, generalized
to arbitrary tracks/clips.

The `AudioClipNode` from RB-1 already takes a `setStart()` + a loaded buffer;
GraphBuilder reuses it. For RB-2, asset decode is eager (whole file in memory);
streaming is later.

## Timeline widget (ImGui)

Hand-rolled, immediate-mode. Layout:
- Left gutter: track names + gain.
- Right area: horizontally-scrollable timeline; clips drawn as rectangles with
  waveforms; playhead is a vertical line.

Interactions (RB-2 scope):
- Scroll/zoom (samples-per-pixel).
- Click a clip to select; drag to move (snaps to samples).
- Click empty timeline to seek transport there.
- Playhead follows transport; transport seeks on click.

State is held in DaveApp; the widget is a pure function of (Edit, viewState).
No widget owns data — that's the document's job.

## Waveform rendering

Peak-based, min/max per pixel column. Computed once per asset (cache keyed by
asset id + zoom bucket). Drawn with `ImDrawList::AddRectFilled` per column
(min→max). For RB-2 this is CPU-computed, ImDrawList-rendered — good to ~50k
visible samples. The GL FBO mipmap renderer is a later optimization when dense
zoom demands it.
