# Fulskrin — multi-layer video mapping (V1)

Video mapping for stage and installation work, on Mac / Windows / Linux.
**Qt 6** interface, **FFmpeg** decoding, **OpenGL 3.3** rendering, effects and generators in the **ISF** format.

## Features

**Layers.** A stack of layers (the top one is drawn on top) with opacity and blend mode
(Normal / Add / Screen / Multiply). Layers are created empty with **+**, then loaded by dropping a media onto
them. One source per layer: video (anything FFmpeg reads, **with its sound**), still image, ISF generator, or
audio file (an audio layer, same transport, no picture). Layers can be renamed, locked (padlock), multi-selected
and grouped (⌘G), groups inside groups included; a group behaves as a layer without a source — ROI, color,
mapping, effects and compositing apply to the composite of its layers. Right-click a layer to **copy its parameters** and paste them onto one or
several others, either all of them or only the source, the ROI, the color, the spatial, the effects or the
compositing.

**A layer as a source.** A layer can take the picture of **another layer** of the composition instead of a file
(Source tab ▸ *Use a layer…*), tapped either **pre-FX** (after that layer's ROI and color, before its effect
chain) or **post-FX** (after it). The same picture is then mapped, colored and treated a second time, somewhere
else on the stage. A layer used this way is rendered even when it is hidden — including a group, which makes a
hidden group a reusable "source" — and anything that would make a picture feed back on itself is refused.

**Transport.** The Source tab shows what the media is (name, resolution, duration, rate, codec, sound), then a
row of transport buttons — play backwards, pause, play, back to the in point, one frame back, one frame on —
a row of play modes with the in / out buttons, and three bars: **Position**, **Speed** and **Loop** (the played
range, two handles). Each bar is one control: drag it, type the value at its right end, or use the wheel; Shift
makes the drag ten times finer and the notable values catch the cursor. The same bar is used throughout —
opacity, volume, ROI, the balance, the color channels, the parameters of every effect and generator. Play / pause is also Space.
The play modes: One-shot (freezes on the last frame), Loop, Ping-pong (decoded backwards in short windows, so any
codec works), Stop (black and silent at the end). **In / out points** per layer bound playback in both
directions, draggable on the playback bar. The default mode for newly loaded media is a setting.

**Picture.** The **ROI** chooses the part of the source picture used (drag the sides of the rectangle).
**Color** works like DaVinci Resolve: balance (Temp −4000…4000, Tint −100…100, luminance kept), then a color
removed (filter) and a color added (light) — `out = balance(in) × (1 − removed) + added` — edited in RGB, HSL,
additive, subtractive or all together. Every color parameter has its own switch, plus one for the whole section:
a switch off keeps its value and simply stops applying it, which makes a before / after easy.
**Spatial** gives position (px) and scale (%, X and Y linked by default) of the mapped layer.

**Mapping.** 4 corners with true perspective (homography) and a deformation mesh from 2×2 to 32×32 points with
Catmull-Rom interpolation; the two combine — align the corners first, refine with the mesh. Points are
multi-selected and moved together. The preview zooms (wheel / − + Fit) for finer moves.

**Effects.** An ISF chain per layer: any number of effects, reorderable, each one switchable, plus a master
switch for the chain. Parameters are generated from each shader's JSON header. **ISF v2** is supported (multiple
passes, computed pass sizes, persistent and float buffers, imported images, `.vs` vertex shaders, events).
The library is scanned in the bundled `isf/` folder, the standard ISF folders (`/Library/Graphics/ISF`,
`~/Library/Graphics/ISF`) and your own.

**Sound.** Each layer with sound has a volume (0–200%), a mute switch and a meter; hiding the layer silences it.
The sound follows the layer's playhead (play, pause, seek, loop, speed — tape-style pitch), stays within a few
milliseconds of the picture, and realigns with a short fade after a dropout or a seek. Everything is mixed to one
stereo output (48 kHz).

**Viewports.** The composition is one space of pixels (its size is set in the **Master** tab); every layer lives
in it and is rendered once. A **viewport** is a window onto it, listed in a block at the top of the layer list:
its Spatial tab places it in the composition (position, scale, *Pixel for Pixel*), or drag its frame in the
preview; like a group it has ROI, color, effects and opacity; its **Output** tab sets its size in pixels, its
screen, how it is shown there (hidden, window, fullscreen) and what it **publishes** — **NDI**, **OMT**,
**Syphon** (macOS), **Spout** (Windows). Three 1920 × 1080 projectors side by side: a 5760 × 1080 composition and
three viewports. Each item at the top of the list chooses the viewports it appears in (Compositing tab, all by
default); inside a group, the group decides. Memories recall that routing and leave the viewports alone. There is
always at least one viewport.

**Output.** ⌘F puts every viewport fullscreen on its own screen, synced to vertical refresh, and a second ⌘F puts
each one back how it was; ⌘⇧F does the same with windows. On macOS a fullscreen output takes a Space of its own,
and the Space is closed when it leaves fullscreen — the window is hidden only once macOS has finished the exit.
With several windows, only the last one presented waits for the vertical refresh, so the frame rate does not drop.
Rendering runs on a dedicated thread, so a slow or blocked interface never interrupts the projected image. The
**Master** tab drives every viewport: level fader, Blackout with fade (⌘B, fades the sound too), audio output and
composition size. Syphon and Spout are built in; NDI and OMT are loaded only when enabled, so Fulskrin runs
without them.

**Media Bin.** Every media used by the project, grouped by type and searchable, with resolution, duration, sound
format and how many layers use it. Missing files are shown in red and **Replace…** relinks them everywhere at
once. An **ISF › Generators** category lists the library's generators, filed by ISF category. Drag an item onto a
layer to load it, or right-click it and pick the layer from **Load into Layer**.

**Memories** (the scenes / cues of MadMapper). A grid of thumbnails storing the state of the layers; a click
shows the content in the inspector, double-click or GO recalls it. Each layer of a memory **unfolds** — source,
ROI, color, mapping, effects and ISF parameters — and **every stored value can be edited there**, without moving
the composition. Numbers fade to the memory's values in its fade time, unless the **Time** column gives one of
them its own: *Transition* (the memory's fade), *Cut*, or a number of seconds — one time for the whole ROI, the
whole mapping, each color control and each ISF parameter. Sources and effect chains switch at once;
a layer deleted since is recreated; unchecked and locked layers are left alone.

**Look.** The accent — the selected layer, the bars, the controls that are on, the links — is a very light grey
by default and is changed in **Settings ▸ Interface ▸ Accent color**; the whole interface follows at once.

**Control and safety.** The whole namespace is published with **OSCQuery** and settable by **OSC** (see below).
**Undo / redo** covers mapping, parameters, layers, effects and source changes. The session **autosaves** every
10 s and is offered back after a crash (in blackout if the output was showing); writes are atomic. Projects are
readable JSON (`.fulskrin`) with paths relative to the project, so a show folder moves between machines.

Bundled shaders: generators TestPattern, Plasma, Gradient, Clouds, SolidColor; effects ColorCorrection, Hue,
Blur, Trails, Kaleidoscope, SoftEdges, Mask, FlipCrop, Pixelate.

## Building

Qt 6.2+, FFmpeg development libraries, CMake 3.21+ and a C++17 compiler. On the first `cmake` run, miniaudio and
the Syphon / Spout sources are downloaded from GitHub, so an internet connection is needed once
(`-DFULSKRIN_SYPHON=OFF`, `-DFULSKRIN_SPOUT=OFF` to skip them).

### macOS

```bash
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
open build/Fulskrin.app
```

To distribute it: `"$(brew --prefix qt)/bin/macdeployqt" build/Fulskrin.app`, then check with
`otool -L build/Fulskrin.app/Contents/MacOS/Fulskrin` that nothing still points to `/opt/homebrew`.
The application is not signed: on first launch, right-click → Open.

### Windows 10/11

Install Qt 6 (MSVC 2022 64-bit) and Visual Studio 2022 (C++), and download a **shared** FFmpeg build with
development files (e.g. `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from github.com/BtbN/FFmpeg-Builds).
In "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64 -DFFMPEG_ROOT=C:\ffmpeg-8.1-shared
cmake --build build --config Release
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe build\Release\Fulskrin.exe
```

The FFmpeg DLLs are copied next to `Fulskrin.exe` automatically.

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev libgl1-mesa-dev \
                 libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev
cmake -S . -B build && cmake --build build -j
./build/Fulskrin
```

## Shortcuts

Ctrl on Windows and Linux, ⌘ on Mac.

| | |
|---|---|
| New (empty) layer | **+** in the layer list, or ⌘⇧N |
| Load / eject a media | drag it onto the layer's row or the Source drop zone / **×** |
| Import to the Media Bin | ⌘I, or drop files onto it |
| Group / ungroup | ⌘G / ⌘⇧G |
| Rename · lock a layer | double-click or F2 · padlock or ⌘L |
| Copy / paste a layer's parameters | right-click the layer |
| Reset a parameter | click its name |
| Accent color of the interface | Settings ▸ Interface |
| Media Bin / Layers panel | ⇧1 / ⇧2 |
| Play / pause the selected layer | Space |
| In / out points at the position | I / O |
| Every viewport fullscreen · in windows (again: back) | ⌘F · ⌘⇧F |
| Blackout (picture and sound) | ⌘B |
| Close the output from the output | ⇧Esc (Esc alone does nothing, for safety) |
| Undo / redo | ⌘Z / ⌘⇧Z |
| Save · discard on quit | ⌘S · ⌘D |
| Zoom · pan the preview | wheel or pinch, − + Fit · middle button or ⌥+drag |
| Select mapping points | ⌘+click, ⌘+drag a rectangle, ⌘A |
| Move points / layer | drag (⇧: fine), or arrow keys (⇧: 10 px) |
| Next / previous handle or field | Tab / ⇧Tab |
| Recall a memory | double-click, Enter or GO |

Suggested alignment workflow: put the **TestPattern** generator on the layer, align the 4 corners (red top left,
green top right, blue bottom left, yellow bottom right), then switch to Mesh for non-flat surfaces.

Tested against the official Vidvox collection ("ISF-Files"): 256 of the 259 generators and effects compile and
render; the other 3 rely on implicit int/uint conversions that standard GLSL rejects. The 68 transitions are
skipped — V1 has no notion of transitions.

## OSC

Enabled by default in **Settings ▸ OSC**: OSC on **UDP 1234**, OSCQuery on **TCP 5678** (HTTP and WebSocket),
the default ports of libossia / score. The server is announced by zeroconf (`_oscjson._tcp`, `_osc._udp`, as
"Fulskrin (&lt;machine&gt;)"). Writes made by OSC are not undoable (show control); the interface follows them.

`GET http://<machine>:5678/` returns the whole tree, `GET /path?VALUE` one attribute, `GET /?HOST_INFO` the
server. Over WebSocket, `{"COMMAND":"LISTEN","DATA":"/path"}` subscribes and value changes come back as binary
OSC; clients get `PATH_CHANGED` when the namespace changes. Address patterns (`/layers/*/opacity 0.5`) and
bundles are supported. Names are made OSC-safe: spaces become `_`, a duplicate name gets `_2`.

| Address | Type | |
|---|---|---|
| `/master/level` · `blackout` · `fade` · `volume` · `mute` · `fps` | f · T · f | picture, blackout, sound, render rate (read only) |
| `/composition/width` · `height` | i | |
| `/layers/<name>/name` · `visible` · `locked` · `opacity` · `blend` · `type` | s · T · T · f · s · s | |
| `…/source/file` · `play` · `restart` · `position` · `speed` · `mode` · `in` · `out` · `duration` | | media and transport |
| `…/source/volume` · `mute` | f · T | sound of the layer |
| `…/source/roi/left` · `top` · `right` · `bottom` | f 0..1 | part of the source picture used |
| `…/source/layer` · `tap` | s | another layer as the source (by name), `prefx` or `postfx` |
| `…/source/params/<input>` | per ISF type | generator parameters |
| `…/color/temp` · `tint` · `add` · `remove` | f · f · fff · fff | −4000..4000 · −100..100 |
| `…/color/enabled` · `tempEnabled` · `tintEnabled` · `addEnabled` · `removeEnabled` | T | color switches |
| `…/spatial/position` · `scale` · `corners/tl` `tr` `br` `bl` | ff | px · % · normalized |
| `…/effects/enabled` · `effects/<effect>/enabled` · `effects/<effect>/<input>` | | effect chain |
| `/layers/<group>/layers/<name>/…` | | layers of a group (and so on, for groups inside groups) |
| `/layers/<name>/viewports/<viewport>` | T | shown in that viewport (items at the top of the list) |
| `/viewports/<name>/width` · `height` · `mode` | i | size in pixels; 0 hidden, 1 window, 2 fullscreen |
| `/viewports/<name>/…` | | name, visible, opacity, ROI, color, spatial, effects, as for a group |
| `/memories/recall` · `/memories/<n>/recall` · `/memories/count` | i · N · i | recall memory n (1 = first) |

A locked layer refuses every write except `visible`, `locked` and the transport.

## Architecture

```
src/engine/   engine, with no widget dependency (QtCore/QtGui/OpenGL + FFmpeg)
  Engine        one class, its methods spread over the six files below by concern:
   · Engine      OpenGL context and render thread, runGl() tasks, output windows, master and blackout
   · Layers      layers, order and groups, sources, transport, effect chain, copy of parameters
   · Render      one frame: sources, layers in dependency order, composite, preview, publishing
   · Project     a layer to and from JSON, saving and opening a .fulskrin
   · Media       file types read, media used by each layer, Media Bin, relinking
   · Memories    memories (snapshots of the layers) and their fades
  Publish       NDI / OMT (loaded at runtime), Syphon (.mm), Spout; asynchronous GPU readback
  Isf           ISF parser and renderer (GLSL 330 core translation, passes, buffers)
  VideoDecoder  FFmpeg decoding on a thread, frame queue, seamless loop, seeking
  AudioStream   FFmpeg audio decoding + resampling on a thread, synced to the layer playhead
  AudioOutput   sound card (miniaudio), mix of all layers, master volume, meters
  Mapping       4-corner homography + Catmull-Rom mesh
  LayerTree     order and grouping of the layers (ids), normalization, moves
  Osc           OSC (UDP) and OSCQuery (HTTP, WebSocket) server
  Zeroconf      DNS-SD announcement (Bonjour on macOS, own mDNS responder elsewhere)
src/ui/       Qt Widgets interface
  MainWindow, LayerInspector, ParamPanel, MappingView, OutputWindow (one per viewport)
  Commands      undo commands (QUndoStack)
  MediaBin, LayerTable, MasterPanel, SettingsPanel, MemoryPanel
  ViewportOutput  Output tab of a viewport: size, screen and mode, publishing
  Widgets       click-to-reset labels, bars that are dragged and typed in one widget (SliderField,
                RangeField), transport and play-mode icons drawn by hand, ROI editor, color editor
isf/          bundled shaders
test/         automated tests
```

The engine owns its OpenGL context on a dedicated thread: it renders the composition, cuts each viewport out of it,
presents each one in its window (vertical sync) and publishes a copy of the whole composition for the interface
preview. The interface reads and modifies layers
under `Engine::Lock`; anything touching OpenGL goes through `Engine::runGl()`, which runs on the render thread.
The interface can therefore be replaced, or the engine driven over OSC, without touching rendering.

## Tests

```bash
test/make_media.sh                         # test media, once
python3 test/make_tests.py                 # test projects (one per shader and codec, plus the UI demo)
cmake --build build --target fulskrin_tests
./build/fulskrin_tests                     # project, video, ISF, undo, render thread, audio, OSC, memories
test/ui_test.sh out/ui                     # Linux + Xvfb + openbox + xdotool: full interface scenario
./build/Fulskrin --render output.png project.fulskrin   # headless render of a project
```

`fulskrin_tests --check-isf <folder>` compiles and renders every shader in a folder.

## Known V1 limitations

- **CPU decoding to RGBA**: fine for a few HD streams. Several 4K streams will need GPU YUV→RGB conversion,
  compressed HAP textures and hardware decoding (VideoToolbox, D3D11VA, VAAPI).
- **Sound**: one stereo output; no multichannel routing, per-layer output, fades or audio effects yet.
- **Viewports are rectangles** of the composition: no per-projector warp or automatic edge blending yet
  (the SoftEdges effect on a viewport helps with manual blending).
- No timeline, MIDI or DMX control; no camera, Syphon / Spout / NDI / OMT **inputs**.
- NDI and OMT send each viewport read back from the GPU (one frame of latency, noticeable CPU load in 4K).
  On Linux, OMT needs `avahi-daemon`, without which Fulskrin refuses to enable it. Syphon is OpenGL-only.
- ISF audio inputs are not supported (black texture).
- Transport and composition size are deliberately not undoable; undoing an effect-chain change reloads the
  layer's effects (Trails buffers restart from zero).
