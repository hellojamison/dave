# Spectral Phase 05 — Integrated Spectrogram Workspace

## Objective

Add a Dave-native workspace that opens a selected audio clip as a synchronized,
GPU-rendered spectrogram. This phase is a viewer only.

## Entry gate

- Phases 00–04 are accepted.
- Valid display tiles can be produced incrementally for a long asset.
- Current timeline and window-resize behavior is covered by headless tests.

## Implementation changes

- Add an explicit `Edit Spectrally` action for a selected audio clip and a
  dedicated central-editor workspace; do not squeeze the editor into a normal
  automation lane.
- Keep Dave's global transport, timing format, markers, and selected clip as the
  synchronization source.
- Add a GUI-only `SpectralEditorViewModel` that records navigation and analysis
  requests. File access and jobs remain outside drawing functions.
- Render magnitude tiles as OpenGL textures with bounded upload work per frame.
- Add horizontal time navigation and vertical linear/log frequency navigation.
- Provide frequency ruler, time ruler, playhead, clip source bounds, overview,
  channel selector, FFT/display controls, amplitude floor/range, and color-map
  control.
- Preserve the visible time/frequency anchor while zooming.
- Use progressive resolution: low-detail tiles appear first and refine without
  moving the view.
- Clearly distinguish analyzing, partial, ready, missing-source, corrupt-cache,
  cancelled, and failed states.
- Make keyboard focus and Escape behavior compatible with existing Dave
  shortcuts.

## Public interfaces

- `SpectralEditorViewState`
- `SpectralEditorViewModel`
- `SpectralDisplayRequest`
- `SpectralViewport`
- `SpectralTextureCache`
- `drawSpectralEditor(...)`

## Tests

- Headless workspace open/close and selected-clip identity.
- Time/frequency coordinate conversion at viewport boundaries.
- Zoom anchoring around mouse and playhead.
- Channel isolation and unavailable-channel states.
- Progressive tile replacement without geometry shift.
- Texture cache eviction and bounded uploads per frame.
- Missing/corrupt/analyzing/error presentation.
- Minimum, normal, and Retina framebuffer layouts.
- Timeline and transport shortcuts still behave correctly when the spectral
  workspace has or lacks keyboard focus.

## Acceptance

- A long clip opens quickly with progressive analysis and bounded UI work.
- Spectral playhead remains sample-aligned with the Dave transport.
- Window resizing changes the central editor without distorting controls.
- Screenshots at minimum and normal sizes are captured and every image is
  inspected for ruler, toolbar, tile seams, text legibility, and clipping.
- Focused UI/cache tests, full headless suite, app link build, and
  `git diff --check` pass.

## Stop conditions

Stop if the UI thread decodes audio, waits for analysis, uploads an unbounded
number of textures per frame, or if visual refinement shifts time/frequency
coordinates.

## Out of scope

No selection, painting, spectral mutation, layers, preview rendering, or ML.
