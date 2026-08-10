# Demo Maker

A small procedural-demo framework built around one shared music engine.
The current prototype already has:

- a sample-accurate step sequencer;
- a polyphonic procedural synth with four editable instrument definitions;
- a playlist music editor with reusable MIDI patterns and piano rolls;
- readable project saving and compact runtime-song export;
- an audio-derived musical clock for visual synchronization;
- an OpenGL fullscreen shader with automatic live reload;
- headless WAV rendering for testing and music iteration.

The authoring tool and tiny exported runtime will both link `tiny_core`, so
composition playback cannot drift between the editor and the final demo.

## Build

Fedora packages:

```bash
sudo dnf install cmake gcc-c++ SDL2-devel freetype-devel mesa-libGL-devel
```

Configure, build, and test:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Music editor

Open or create a project:

```bash
./build/tiny_editor --project my-song.tds --effect starfield.fxp
```

The default screen is the playlist/arrangement. A pattern is reusable MIDI
content for one instrument; a playlist clip is only a reference to that
pattern. The workflow is:

1. Select, create, or duplicate a pattern block in the pattern bar.
2. Drag the colored pattern block into a playlist lane.
3. Drag placed clips to move them through time or between lanes.
4. Hold Shift while dragging a selected clip to copy and move the complete
   selection as a linked group. Ordinary dragging also moves the selection as
   a group.
5. Drag a box from empty playlist space to select multiple clips; hold Shift
   while boxing to add clips to the existing selection.
6. Double-click a clip (or press `Edit Pattern`) to open its piano roll.
7. Return to the playlist after editing the pattern.

Editing a piano roll updates every playlist clip that references that pattern.
While the piano roll is open, only that pattern and its instrument are played;
it loops at the pattern boundary. Returning to the playlist restores the full
arrangement near the position where pattern editing began.
Playlist playback automatically loops from the first occupied bar through the
last occupied bar. The range is shown as `AUTO LOOP`, updates when clips move,
and is carried into demo playback, effect preview, and offline WAV rendering.
Right-click a playlist clip to remove it. The playlist fits the complete
16-bar song into one DAW-style arrangement overview, while `AUTO LOOP` marks
the actually populated bar range.
Each lane has a `MUTE` checkbox. Muting a lane silences its arranged clips
without changing the reusable pattern or its piano-roll solo preview.

The `FX` lane at the top of the arrangement holds shader blocks on the same
bar grid as the MIDI clips. `+ BLOCK` adds a one-bar Starfield block. Drag a
shader block to move it, Shift-drag to copy it, right-click to delete it, or
drag its bright right-edge handle to change its duration. FX movement and
right-edge resizing snap to the one-beat grid, with a minimum one-beat length.
Selecting a block opens the `EFFECT` inspector. The shader and its pixel text
render only while the playhead is inside an enabled FX block. Visual blocks are
stored in the `.fxp` preset and participate in `AUTO LOOP`; older presets
without blocks automatically receive one spanning the populated song.

Inside the piano roll:

- left-click or left-drag to paint notes;
- right-click or right-drag to erase notes;
- use `Z` through `M` as a one-octave piano keyboard;
- use the bottom scrollbar to navigate pattern time;
- use the right scrollbar or mouse wheel to navigate the MIDI pitch range;
- hold Ctrl while using the mouse wheel to zoom around the cursor in both time
  and pitch;
- use `[` and `]` or the octave buttons to change the computer-keyboard input
  octave without moving the viewport;
- use the arrow keys to move or tune a selected note;
- press Escape to return to the playlist.

The right-hand DAW inspector has `INSTRUMENT`, `EFFECT`, and `TEXT` tabs.
Instrument edits the selected pattern's waveform, envelope, filter, pitch
drop, gain, and pan. Effect embeds the live shader preview and generated
uniform controls beside the timeline. Text places multiple hard-edged bitmap
text overlays directly over that preview. All three use the same transport
clock as the arrangement.

`Remove end clicks` adds a short safety fade and allows release tails to finish
across pattern-loop boundaries; it is stored per instrument and included in
runtime exports. Space controls playback. `Ctrl+S` and the Save button save
both the `.tds` song and `.fxp` visuals; `Ctrl+O` loads both, and `Ctrl+E`
exports the runtime song.

Save produces a readable `.tds` project containing patterns, instruments, and
playlist clips. Legacy flat projects are imported as patterns automatically.
Export flattens the playlist into a compact `.bin` runtime song beside it. Run
that exact arrangement in the demo with:

```bash
./build/tiny_demo --song my-song.bin
```

## Demo runtime

Run the audiovisual prototype with its built-in song:

```bash
./build/tiny_demo
```

The fragment shader at `shaders/demo.frag` reloads automatically when saved.
Press Space to pause, R to force a reload, and Escape to quit.

### First effect: 3D star flight

The current shader distributes deterministic stars through a bounded 3D
volume and moves the camera forward through it. Perspective provides the
outward acceleration; near and far fades make the limited view distance
visible; nearby stars receive short motion streaks. A slow curved camera path
keeps the flight from feeling like a flat zoom.

The main tuning parameters are declared at the top of `shaders/demo.frag`:

- `u_star_count` controls density and fragment-shader cost;
- `u_camera_speed` controls forward travel;
- `u_near_plane` controls how close stars get before disappearing;
- `u_view_distance` controls the depth of the generated volume.

### Integrated visual editor

Shader and text editing are built into the main DAW:

```bash
./build/tiny_editor --project song.tds --effect starfield.fxp
```

Select a shader block in the `FX` arrangement lane to open `EFFECT` in the
right inspector and see the transport-synchronized preview and parameter
pages. The interface is generated from declarations inside the shader. For
example:

```glsl
// @param u_camera_speed "Camera speed" 0.115 0.01 0.4 0.005
uniform float u_camera_speed;
```

The annotation contains the uniform name, display label, default, minimum,
maximum, and step. Adding another annotation and uniform automatically adds
another editor slider after the shader reloads.

The integrated visual inspector supports:

- live uniform editing against the actual OpenGL renderer;
- automatic GLSL reload while preserving current parameter values;
- default restoration and readable `.fxp` preset save/load;
- multiple hard-edged Amiga-style pixel text overlays;
- the same audio-derived beat clock as the final demo;
- direct use of the open DAW arrangement, including lane mutes;
- unified Save and Load actions for music and visuals.

Select the `TEXT` tab and press `ADD` to create an overlay. Click its text
field to type, then press Enter or Escape to finish. Each overlay has visible,
position X/Y, integer pixel size, RGB, and `AUTO CENTER` controls. Auto Center
uses the actual pixel-font bounds to center the complete text and its shadow.
Text is rendered with the same built-in bitmap font in the live preview and
`tiny_demo`, with no system font or smoothing dependency. Pixel size is defined
on the 1280×720 demo canvas and scales proportionally in the embedded preview
and when the runtime window is resized. Saving the `.fxp` preset stores every
overlay.

Use a saved preset in the demo runtime:

```bash
./build/tiny_demo --effect starfield.fxp --song my-song.bin
```

Render the procedural song without opening a window or audio device:

```bash
./build/tiny_demo --render-wav tiny-demo.wav 16 --song my-song.bin
```

## Architecture

```text
tiny_core
├── StudioProject: reusable MIDI patterns and playlist clips
├── Song: flattened runtime events, instruments, BPM, and musical clock
├── Project I/O: readable projects and compact runtime blobs
└── SynthEngine: sequencer, voices, envelopes, oscillators, filters

tiny_demo
├── SDL2 audio callback
├── audio-clocked synchronization
└── OpenGL shader renderer

tiny_editor
├── playlist arrangement and draggable MIDI pattern clips
├── layered per-pattern piano roll
├── live instrument controls
├── embedded OpenGL preview, shader controls, and pixel text
├── unified music/visual save and load
├── project export
└── links the same tiny_core library
```

`src/song.cpp` supplies the initial demo song. Once a project is saved, the
editor-owned `.tds` file becomes the composition source and its `.bin` export
is what the demo consumes.

The next editor milestone is arrangement length, track solo, copy/paste,
undo/redo, and a visual-preview panel driven by the same musical clock.
