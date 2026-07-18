# RB-3 Plugin Host — Design Notes

## Goal

Host third-party VST3 plugins (and later AU + CLAP) inside Dave, processing
audio in our node graph. End-to-end: user adds a plugin to a track → its
editor opens → audio flows clip → plugin → output, with parameters automatable.

This is the long pole (~5-6 months in the rebuild plan). The dominant cost is
the host implementation against the raw VST3 SDK — JUCE gives this away, we
build it.

## Why VST3 first (not AU or CLAP)

- **VST3 has the biggest plugin ecosystem** — most users' plugin folders are VST3.
- **The SDK is MIT since October 2025** (Steinberg relicensed) — no license fee,
  no signed agreement, clean for our dual-license path.
- **Cross-platform** — the host code we write for VST3 (scanner, loader,
  processor wrapper, editor embedding) largely transfers to AU/CLAP later.
- CLAP is actually simpler (cleaner API, MIT) but has fewer plugins. AU is
  macOS-only. VST3 first maximizes early usefulness.

## The hard parts (and how we sequence around them)

1. **Plugin lifecycle / threading.** VST3 has strict rules: create on the main
   thread, set state before processing, never call the editor from the audio
   thread. We isolate ALL of this inside a `PluginInstance` class that exposes
   a simple RT-safe `process(in, out, midiIn, midiOut, time)` — the graph's
   PluginNode calls only that.

2. **The audio thread ↔ plugin boundary.** VST3's `IComponent::process` runs on
   our RT thread. We must hand it correctly-laid-out `ProcessData` (channel
   pointers, sample counts, parameter changes, MIDI). This is fiddly but
   bounded — one adapter class.

3. **Editor window embedding.** The plugin returns a platform view (NSView on
   macOS, HWND on Windows) that we must parent into our window. On macOS this
   means taking an NSView from the plugin and adding it as a subview of our
   GLFW window's contentView. This is the genuinely platform-specific, easy-to-
   get-wrong part. We DEFER it past the first working version — get audio
   processing working headless first, then tackle the GUI embedding.

4. **Out-of-process sandboxing.** A crashing plugin shouldn't kill Dave. The
   real fix is running each plugin in a separate helper process with audio over
   shared memory. That's a huge complexity multiplier. **RB-3 ships in-process**;
   sandboxing is post-1.0.

## Sequencing (deliver value early, not after 6 months)

1. **SDK builds.** Add the VST3 SDK, get `base` + `pluginterfaces` + `public.sdk`
   compiling. (This alone can be painful — the SDK's CMake is finicky.)
2. **Scanner.** Walk `/Library/Audio/Plug-Ins/VST3` and `~/Library/.../VST3`,
   enumerate `.vst3` bundles, cache name + UID + category. In-process for now.
3. **Headless instance + audio.** Load one plugin, set up ProcessData, call
   process() from a PluginNode. **First milestone: hear a plugin process our
   clip's audio** (e.g. a free EQ visibly changes the sound). No GUI yet.
4. **Parameters.** Enumerate params, expose them, host-side set (RT-safe queue).
5. **Editor embedding.** Parent the plugin's NSView into our window. **Second
   milestone: tweak a knob in the plugin GUI, hear it change.**
6. **Persistence.** Save/restore plugin instance + state chunk in the Edit.

We ship (3) as an internal milestone even though there's no UI — it proves the
hard part works. Then layer UI + GUI on top.

## Module layout

```
src/engine/plugins/
  PluginHost.h/cpp        — scanner + cache (UI thread)
  PluginInstance.h/cpp    — one loaded VST3: lifecycle, ProcessData adapter,
                             RT-safe process() entry point
  PluginNode.h/cpp        — graph Node wrapping a PluginInstance
  PluginParam.h/cpp       — parameter normalization + RT queue (later)
```

## Threading contract (critical)

- `PluginHost` / `PluginInstance::load` / parameter queries: **main thread only**.
- `PluginInstance::process`: **RT thread only**. Lock-free parameter queue in.
- Editor create/destroy: **main thread only**.
- We never hold a lock the audio thread needs.
