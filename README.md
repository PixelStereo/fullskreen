# Fulskrin — multi-layer video mapping (V1)

Video mapping application for stage and installation work, on Mac / Windows / Linux.
**Qt 6** interface, **FFmpeg** decoding, **OpenGL 3.3** rendering, effects and generators in the **ISF** format.

## What V1 does

- **Stacked layers** (the top layer is drawn on top), opacity, blend modes Normal / Add / Screen / Multiply.
- **Groups of layers**: the ⊞ button (Ctrl+G / ⌘G) creates a group; the selected layers go straight into it.
  Layers are then dragged onto a group (or between its layers) to go into it, and out of it the same way.
  A group folds and unfolds in the layer list (arrow). A group has the properties of a layer without a source:
  crop, color, spatial (mapping), effects and compositing apply to the composite of its layers.
  Ungroup: Ctrl+Shift+G. Deleting a group deletes its layers; duplicating it duplicates them.
- **Lock**: the padlock between the visibility box and the name (or Ctrl+L) forbids any edit of the layer
  (properties, mapping, source, effects, name, deletion, moves); the visibility and the transport stay available.
  A locked group locks its layers.
- **Rename** a layer or a group by double-clicking its name in the list (or F2), or in the inspector.
- **Click on a parameter's name** (opacity, speed, volume, position, scale, ISF parameters, color channels, crop…)
  to put it back to its default value.
- **One source per layer**:
  - video (H.264, HEVC, ProRes, HAP, DNxHD… anything FFmpeg reads), play / pause / speed / position,
    **with its sound** when the file has an audio track;
  - still image;
  - ISF generator;
  - **audio file** (WAV, AIFF, MP3, AAC/M4A, FLAC, Ogg/Opus… anything FFmpeg reads): an audio layer, with the same
    transport as a video (play / pause / play mode / speed / position) and no picture.
- **Play modes** for videos and sounds, four exclusive buttons in the Source tab: **One-shot** (plays once,
  freezes on the last frame), **Loop**, **Ping-pong** (forwards then backwards — the picture and the sound are
  decoded backwards in short windows, so any codec works, all-intra codecs such as HAP or ProRes being the
  lightest), **Stop** (plays once, then black and silent). The mode given to newly loaded media is set in the
  **Settings** tab (Loop by default).
- **In / out points** per video or sound layer: playback, loops and ping-pong stay within them, in both
  directions; One-shot / Stop end at the out point (the in point backwards). Saved in the project.
  They are shown on the playback bar of the Source tab (highlighted range between [ ] markers), and the markers
  can be dragged there.
- **Crop**: in the Source tab of a layer or a group, a preview of the source picture with a rectangle whose sides are
  dragged (they stay straight) chooses the part of the picture used — the whole picture by default.
- **Color tab**: first the **balance**, as in DaVinci Resolve: **Temp** (blue −4000 … yellow +4000) and
  **Tint** (green −100 … magenta +100), luminance kept; then a color removed from the picture (filter) and a color
  added to it (light): out = balance(in) × (1 − removed) + added. The colors are edited in RGB, HSL, additive
  (R G B light), subtractive (C M Y filters) or all of them together (linked), chosen per layer (Edit in).
- **Negative speed** plays videos and sounds backwards, in every play mode (Loop goes on backwards from the end,
  One-shot stops on the first frame…); changing direction keeps the current position.
- **Spatial tab**: position (center, composition pixels) and scale (% of the composition, X and Y linked by
  default) of the whole mapped layer — corners and mesh are transformed together and follow handle edits.
- **Preview zoom**: mouse wheel / pinch around the cursor, − / + / Fit buttons; the higher the zoom, the finer
  the moves of the layer and its points.
- **ISF effect chain per layer**: added from a searchable list (Add Effect…: type, then Enter), any number of effects, reorderable, each one can be enabled or disabled,
  and a general switch at the top of the Effects tab turns the whole chain on or off.
  Parameters are generated automatically from each shader's JSON header.
- **ISF v2 support**: multiple passes, computed pass sizes (`"$WIDTH/2"`), persistent and float buffers,
  imported images, secondary image inputs, `.vs` vertex shaders, events, `IMG_PIXEL` / `IMG_NORM_PIXEL` /
  `IMG_THIS_PIXEL` / `IMG_SIZE`. The library is scanned in the standard ISF folders plus your own folders.
- **Mapping per layer**:
  - 4 corners with true perspective (homography);
  - deformation mesh from 2×2 to 32×32 points, smooth interpolation (Catmull-Rom);
  - both combine: align the corners first, then refine with the mesh.
- **Fullscreen output** on the chosen screen, synced to vertical refresh: **⌘F / Ctrl+F** enters and leaves
  fullscreen (on the second screen if one is connected, otherwise on the main screen), also from the output itself.
  ⌘⇧F / Ctrl+Shift+F: windowed output.
- **Rendering on a dedicated thread**: the engine renders and presents the output itself; a slowdown or freeze
  of the interface (loading, dialog, menu) does not interrupt the projected image.
- **Sound**: every layer with sound (audio layer, or video with an audio track) has a volume (0–200%, undoable),
  a Mute switch and a level meter; hiding the layer silences it too. The sound follows the layer's playhead:
  play, pause, seek, loop and speed (tape-style: the pitch follows the speed). Synchronization with the picture
  is kept within a few milliseconds, small drifts are corrected inaudibly, and after an audio dropout or a seek
  the sound realigns with a short fade. All layers are mixed to one stereo output (48 kHz).
  The blackout (Ctrl+B) fades the sound out with the picture; the master fader only acts on the picture.
- **Master tab** (next to the Layer tab): master level fader and **Blackout** button with fade
  (adjustable duration, Ctrl+B / ⌘B, works from the output window too; it fades the sound out too), output screen and mode,
  **audio output** (sound card, master volume, mute, stereo meters), composition size, output publishing.
- **Output publishing** to other software or machines:
  - **NDI** (network): requires NDI Tools or the NDI Runtime (ndi.video) installed on the machine;
  - **OMT** — Open Media Transport (network, open source): requires `libomt` and `libvmx`
    (binaries at github.com/openmediatransport/libomtnet/releases) next to the application,
    in `/usr/local/lib`, or in the folder set in the Master tab;
  - **Syphon** (macOS) and **Spout** (Windows): direct texture sharing on the GPU,
    built into Fulskrin (nothing to install).
  NDI and OMT are loaded when enabled: Fulskrin runs without them. The Master tab shows the state of each one
  (active, number of receivers, or what is missing). Settings are saved in the project.
- **Media Bin** (left, with a search field): every video, image and audio file used by the project, grouped by type, with resolution,
  duration, sound format and the number of layers using them (sources and shader images). **Missing** files are shown in red;
  **Replace…** relinks them (a single replacement fixes every layer, and can be undone).
  An **ISF › Generators** category lists the generators of the ISF library (and those used by layers),
  filed by their ISF category (Alignment, Generative, Noise…).
  Drag an item onto a layer to load it; double-click loads it into the selected layer.
- **Layers are created empty** with **+**, then loaded with whatever is dropped onto them: a video, an image,
  a sound or an ISF generator, from the Media Bin or the Finder / Explorer — onto the layer's row in the list,
  or onto the drop zone of its Source tab. An ISF effect dropped onto a layer joins its effect chain.
  The **×** next to the drop zone ejects the media.
- **Left panel, two tabs**: **Media Bin** and **Layers** (Shift+1 / Shift+2; dragging a media over the Layers tab
  opens it). The layer list shows visibility, lock, name, number of effects (their names on hover), opacity adjustable
  directly in the row, blend mode, playback position. Several layers can be selected (Ctrl/⌘ or Shift + click).
- **Memories** at the bottom of the window, as the scenes / cues of MadMapper: a grid of thumbnails ending with **+**,
  which stores the current state of the layers. A click selects a memory and shows its content in the inspector
  next to the grid (name, fade time, layers with their visibility, opacity and source); double-click, Enter or GO
  recalls it (undoable). Opacity, volume, crop, color, mapping and ISF numbers fade to the memory's values in its fade
  time; sources and effect chains change at once; a layer shown by the memory fades in, a hidden one fades out;
  a layer deleted since is recreated. Unchecked layers, and locked ones, are left alone. Update stores the current
  state into a memory. Memories are saved in the project and recalled by OSC.
- **Settings tab** (next to Layer and Master; also Preferences…, ⌘, / Ctrl+,): default play mode, color widgets of
  the Color tab for new layers (each layer then keeps its own choice, Color tab ▸ Edit in), OSC on / off and ports.
  Applied immediately.
- **Layer tab** with sub-tabs: **Source** (drop zone, transport, sound, generator parameters, crop),
  **Color**, **Spatial** (mapping), **Effects** (ISF chain), **Compositing** (opacity, blend).
- **OSC control and OSCQuery**: the whole namespace (master, composition, every layer and group: source,
  transport, ISF parameters, crop, color, spatial, effects, compositing) is published with OSCQuery and can be
  set by OSC. See [OSC](#osc).
- **Undo / redo** (Ctrl+Z / Ctrl+Shift+Z, ⌘ on Mac): mapping (handles, arrow keys, buttons), ISF parameters,
  opacity, blend, visibility, name, adding / deleting / reordering layers, effects, source changes.
- **Autosave and recovery**: the session is saved every 10 s when it changes.
  After a crash, Fulskrin offers to restore it at launch; if the output was showing,
  it comes back on the projector **in blackout**, and the operator brings it back up with Ctrl+B.
  Saves are atomic: a crash while writing does not corrupt the project.
- **Projects** `.fulskrin` (readable JSON), with paths relative to the project so a show folder can move between machines.
  A file missing when the project opens is not lost: its path is kept and flagged in the Media Bin.
- **Drag and drop** of files from the system: onto a layer (loaded into it), onto the Media Bin (imported),
  elsewhere in the window (loaded into the selected layer; extra files go to the Media Bin).

Bundled shaders (`isf/` folder): generators TestPattern, Plasma, Gradient, Clouds, SolidColor;
effects ColorCorrection, Hue, Blur (2 passes), Trails (persistent buffer), Kaleidoscope, SoftEdges, Mask, FlipCrop, Pixelate.

## Building

On the first `cmake` run, **miniaudio** (sound output, all platforms) and the **Syphon** (macOS) or **Spout** (Windows) sources are downloaded from GitHub
and built with Fulskrin, so an internet connection is needed the first time.
To skip Syphon or Spout: `-DFULSKRIN_SYPHON=OFF` or `-DFULSKRIN_SPOUT=OFF`.

You need Qt 6.2 or newer, FFmpeg (development libraries), CMake 3.21+ and a C++17 compiler.

### macOS (Intel or Apple Silicon)

```bash
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
open build/Fulskrin.app
```

To distribute the application to another machine: `"$(brew --prefix qt)/bin/macdeployqt" build/Fulskrin.app`,
then check with `otool -L build/Fulskrin.app/Contents/MacOS/Fulskrin` that no library still points to `/opt/homebrew`.
The application is not signed: on first launch, right-click → Open.

### Windows 10/11

1. Install **Qt 6** (online installer from qt.io, "MSVC 2022 64-bit" component) and **Visual Studio 2022** (C++).
2. Download a **shared** FFmpeg build with development files, for example
   `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from github.com/BtbN/FFmpeg-Builds/releases.
   The extracted folder contains `bin/`, `include/`, `lib/`.
3. In "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64 -DFFMPEG_ROOT=C:\ffmpeg-8.1-shared
cmake --build build --config Release
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe build\Release\Fulskrin.exe
```

The FFmpeg DLLs are copied next to `Fulskrin.exe` automatically. Adjust the paths to your versions.

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev libgl1-mesa-dev \
                 libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev
cmake -S . -B build && cmake --build build -j
./build/Fulskrin
```

## Usage

| Action | How |
|---|---|
| New layer | **+** button of the layer list, or Layer ▸ New Layer (Ctrl+Shift+N): the layer is empty |
| Load a media into a layer | drag it (Media Bin, Finder) onto the layer's row, or onto the Source tab drop zone |
| Eject the media of a layer | **×** next to the drop zone (Source tab) |
| Import to the Media Bin | Import… button, Ctrl+I (⌘I), or drop files / a folder onto the Media Bin |
| Relink a moved file | Media Bin ▸ select the red file ▸ Replace… |
| Choose the projector screen | Master tab ▸ Video Output, or Output menu ▸ Output Screen |
| Fullscreen / back | Ctrl+F (⌘F on Mac) |
| Windowed output | Ctrl+Shift+F (⌘⇧F on Mac) |
| Fade to blackout / back | Ctrl+B (⌘B), or the Blackout button in the Master tab |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z or Ctrl+Y (⌘Z / ⌘⇧Z) |
| Media Bin / Layers tab | Shift+1 / Shift+2 |
| Store / recall a memory | + in the Memories grid / double-click, Enter or GO |
| Next / previous field of an effect, handle of the mapping | Tab / Shift+Tab |
| Group layers | select them, then ⊞ or Ctrl+G (⌘G); empty group: click the empty area of the list first |
| Move layers into / out of a group | drag them onto the group's row, or between rows (below a group's last layer, at the left: out of it) |
| Fold / unfold a group | arrow before its name |
| Ungroup | Layer ▸ Ungroup (Ctrl+Shift+G) |
| Rename a layer | double-click its name in the list, or F2 |
| Lock / unlock a layer | padlock in the list, in the inspector, or Ctrl+L (⌘L) |
| Reset a parameter | click its name |
| Part of the source picture used | Source tab ▸ Crop: drag the sides of the rectangle |
| Add / remove color | Color tab (RGB, HSL, additive, subtractive or all) |
| All effects on / off | Effects tab ▸ Effects enabled |
| Close the output from the output | Shift+Esc (Esc alone does nothing, for safety) |
| Match the composition to the projector | Master tab ▸ Composition ▸ "= output screen" |
| Publish via NDI, OMT, Syphon, Spout | Master tab ▸ Output Publishing |
| Play / pause the selected video or audio layer | Space |
| Play mode of a video or a sound | Source tab ▸ One-shot / Loop / Ping-pong / Stop |
| Default play mode for newly loaded media | Settings tab (⌘, on Mac, Ctrl+, elsewhere) |
| Play backwards | negative speed (Source tab), e.g. −1 × |
| In / out points of a video or a sound | I / O keys at the current position, Source tab ▸ In / Out (↺: whole media), or drag the [ ] markers of the playback bar |
| Zoom the preview (finer moves) | mouse wheel or pinch, or the − / + / Fit buttons at the top right of the preview |
| Pan the zoomed preview | middle button or Alt/⌥ + drag, two fingers on a trackpad |
| Position and scale of the whole layer | Layer tab ▸ Spatial ▸ Position X / Y (px), Scale X / Y (%, linked by default) |
| Move a handle | drag (Shift: fine movement) |
| Select several points (mesh or corners) | Ctrl/⌘+click to add or remove, Ctrl/⌘+drag a rectangle, Ctrl/⌘+A for all |
| Move the selected points together | drag one of them, or arrow keys |
| Move the whole layer | drag inside the layer |
| Nudge by one pixel | arrow keys (Shift: 10 px) |
| Next / previous handle / deselect | Tab / Shift+Tab / Esc |
| Corners ↔ mesh mode | Layer tab ▸ Spatial |
| Reload a shader edited in an external editor | ⟳ button in the inspector |

Suggested alignment workflow: put the **TestPattern** generator on the layer, align the 4 corners
(the colored corners show the orientation: red top left, green top right,
blue bottom left, yellow bottom right), then switch to Mesh mode for non-flat surfaces.

ISF folders scanned automatically: the bundled `isf/` folder, `/Library/Graphics/ISF` and `~/Library/Graphics/ISF`
on Mac (where VDMX and other software install their shaders), plus those added with
ISF Library ▸ Add ISF Folder….

Compatibility tested against the official Vidvox collection (GitHub repository "ISF-Files"):
256 of the 259 generators and effects compile and render. The other 3 (Random Characters,
Tiny Date Time Overlay, Line Group) rely on implicit int/uint conversions that standard GLSL rejects.
The 68 ISF transitions are skipped: V1 has no notion of transitions between media.

## OSC

Enabled by default in **Settings ▸ OSC**: OSC messages on **UDP 1234**, OSCQuery on **TCP 5678**
(HTTP and WebSocket on the same port) — the default ports of libossia / score.
The server is **announced by zeroconf** (`_oscjson._tcp` for OSCQuery, `_osc._udp` for OSC, as "Fulskrin (<machine>)"):
Bonjour on macOS, a built-in mDNS responder elsewhere (it shares port 5353 with Avahi / Windows). Writes made by OSC are not undoable (show control); the interface follows them.

- `GET http://<machine>:9001/` returns the whole tree (OSCQuery JSON: `FULL_PATH`, `CONTENTS`, `TYPE`, `VALUE`,
  `RANGE`, `ACCESS`, `DESCRIPTION`, `CLIPMODE`); `GET /path?VALUE` one attribute; `GET /?HOST_INFO` the server.
- WebSocket: `{"COMMAND":"LISTEN","DATA":"/path"}` / `IGNORE`; value changes are sent as binary OSC messages,
  and OSC messages can be sent as binary frames. When layers, sources or effects change (a project is opened,
  a layer added or renamed…), the clients receive `PATH_CHANGED` and fetch the tree again.
- OSC address patterns are supported (`/layers/*/opacity 0.5`), as well as bundles.

Main addresses (layer and effect names are made OSC-safe: spaces become `_`; a duplicate name gets `_2`):

| Address | Type | |
|---|---|---|
| `/master/level` · `/master/blackout` · `/master/fade` | f · T · f | picture fader; blackout (picture and sound); fade time |
| `/master/volume` · `/master/mute` · `/master/fps` | f · T · f | sound output; render rate (read only) |
| `/composition/width` · `/composition/height` | i | |
| `/layers/<name>/name` · `visible` · `locked` · `opacity` · `blend` · `type` | s · T · T · f · s · s | |
| `/layers/<name>/source/file` · `play` · `restart` · `position` · `speed` · `mode` · `in` · `out` · `duration` | | media and transport |
| `/layers/<name>/source/volume` · `mute` | f · T | sound of the layer |
| `/layers/<name>/source/crop/left` · `top` · `right` · `bottom` | f 0..1 | part of the source used |
| `/layers/<name>/source/params/<input>` | per ISF type | ISF generator parameters |
| `/layers/<name>/color/temp` · `color/tint` | f | −4000..4000 · −100..100 |
| `/layers/<name>/color/add` · `color/remove` | fff | |
| `/layers/<name>/spatial/position` · `scale` · `corners/tl` `tr` `br` `bl` | ff | px · % · normalized |
| `/layers/<name>/effects/enabled` · `effects/<effect>/enabled` · `effects/<effect>/<input>` | | effect chain |
| `/layers/<group>/layers/<name>/…` | | layers of a group |
| `/memories/recall` · `/memories/<n>/recall` · `/memories/count` | i · N · i | recall memory n (1 = first) |

A locked layer refuses every write except `visible`, `locked` and the transport (`play`, `restart`, `position`).

## Architecture

```
src/engine/   engine, with no widget dependency (QtCore/QtGui/OpenGL + FFmpeg)
  Memories      memories (snapshots of the layers) and their fades
  Engine        composition, render thread, output presentation, JSON project, Media Bin media
  Publish       publishing: NDI / OMT (loaded at runtime), Syphon (.mm), Spout; asynchronous GPU readback
  Isf           ISF parser and renderer (GLSL 330 core translation, passes, buffers)
  VideoDecoder  FFmpeg decoding on a thread, frame queue, seamless loop, seeking
  AudioStream   FFmpeg audio decoding + resampling on a thread, synchronized to the layer playhead
  AudioOutput   sound card (miniaudio), mix of all layers, master volume, meters
  Mapping       4-corner homography + Catmull-Rom mesh
  LayerTree     order and grouping of the layers (ids), normalization, moves
  Osc           OSC (UDP) and OSCQuery (HTTP, WebSocket) server, namespace built from the engine
  Zeroconf      DNS-SD announcement (Bonjour on macOS, own mDNS responder elsewhere)
src/ui/       Qt Widgets interface
  MainWindow, LayerInspector, ParamPanel, MappingView (editing), OutputWindow (projector)
  Commands      undo commands (QUndoStack)
  MediaBin, LayerTable, MasterPanel   Media Bin, layer list, Master tab
  SettingsPanel Settings tab (preferences of this machine)
  MemoryPanel   memories grid and inspector
  Widgets       click-to-reset labels, playback bar with in / out, crop editor, color editor
isf/          bundled shaders
test/         automated tests
```

The engine owns its OpenGL context on a dedicated thread: it renders the composition, presents it in the output
window (vertical sync) and publishes a copy for the interface preview.
The interface reads and modifies layers under `Engine::Lock`; anything touching OpenGL (shader compilation,
texture release) goes through `Engine::runGl()`, which runs on the render thread.
The interface can therefore be replaced, or the engine driven over OSC, without touching rendering.

## Tests

```bash
test/make_media.sh                         # test media, once
python3 test/make_tests.py                 # test projects (one per shader and codec, plus the UI demo)
cmake --build build --target fulskrin_tests
./build/fulskrin_tests                     # project, video, ISF, undo, render thread, master
test/ui_test.sh out/ui                     # Linux + Xvfb + openbox + xdotool: full interface scenario
./build/Fulskrin --render output.png project.fulskrin   # headless render of a project
```

`test/make_media.sh` creates the test media with FFmpeg (H.264, ProRes, HAP, image); `test/make_tests.py`
generates one project per shader and per codec, plus the demo used by the UI test. `fulskrin_tests --check-isf <folder>` compiles and renders every shader in a folder.

## Known V1 limitations (and next steps)

- **CPU decoding to RGBA**: comfortable for a few HD streams. Several 4K streams will need
  YUV→RGB conversion on the GPU, compressed HAP textures (DXT) uploaded directly to the GPU,
  and hardware decoding (VideoToolbox on Mac, D3D11VA on Windows, VAAPI on Linux).
- **Sound**: one stereo output; no multichannel routing, per-layer output assignment, fades or audio effects yet.
- **Single output**: no multiple outputs, per-projector slicing or automatic edge blending yet
  (the SoftEdges effect helps with manual blending).
- **No timeline, cues, MIDI or DMX control.**
- Groups have one level (no group inside a group).
- **No Syphon / Spout / NDI / OMT inputs, and no camera input** (the output can be published, though).
- NDI and OMT send the image at composition size, read back from the GPU (one frame of latency);
  for a 4K composition expect a noticeable CPU load, especially with OMT, which encodes the image.
- On Linux, OMT needs the Avahi service (`avahi-daemon`) for discovery; Fulskrin refuses to enable it without Avahi
  (the library would abort the program).
- Syphon is built OpenGL-only (no Metal server).
- ISF audio inputs are not supported (black texture).
- Video transport (play, pause, position) is deliberately not undoable; neither is the composition size.
- Undoing a change to the effect chain reloads the layer's effects (Trails buffers restart from zero).
