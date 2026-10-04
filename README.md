# Fulskrin — multi-layer video mapping (V1)

Video mapping for stage and installation work, on Mac / Windows / Linux.
**Qt 6** interface, **FFmpeg** decoding, **OpenGL 3.3** rendering, effects and generators in the **ISF** format.

## Features

**Layers.** A stack of layers (the top one is drawn on top) with opacity and blend mode
(Normal / Add / Screen / Multiply / Subtract / Difference). Layers are created empty with **+**, then loaded by dropping a media onto
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
opacity, volume, ROI, the balance, the color channels, the parameters of every effect and generator.
The play modes: One-shot (freezes on the last frame), Loop, Ping-pong (decoded backwards in short windows, so any
codec works), Stop (black and silent at the end). **In / out points** per layer bound playback in both
directions, draggable on the playback bar. The default mode for newly loaded media is a setting.

**Video playback.** Frames reach the GPU as the decoder gives them — YUV planes (8 to 16 bits, every
subsampling), NV12 / P010, RGB, grey, with or without alpha — and the GPU converts them (the stream's matrix and
range, chroma siting): the processor only decodes. Each decoder copies its frames straight into upload buffers of
the GPU on its own thread; the render thread only hands them over. **Hardware decoding** (VideoToolbox on macOS,
Direct3D 11 / DXVA2 on Windows, VA-API on Linux) is used when the codec allows it (Settings ▸ Playback, on by
default). **HAP**, every variant — Hap, Hap Alpha, Hap Q, Hap Q Alpha, Hap Alpha-Only, Hap R, Hap HDR — goes to the
GPU still compressed (DXT, RGTC, BPTC): only its Snappy stage is undone, its chunks side by side. macOS's OpenGL has
no BPTC, so Hap R and Hap HDR are decoded by the processor there (HDR values above 1 are clipped). A hidden layer
keeps playing and sounding, but is neither uploaded nor rendered. The Source tab says how the frames travel
(*Picture*: `yuv420p · BT.709`, `Hap Q · YCoCg DXT5`, or *converted on the CPU* for the rare layouts the GPU
does not take).

**Picture.** The **ROI** chooses the part of the source picture used (drag the sides of the rectangle).
**Color** works like DaVinci Resolve: balance (Temp −4000…4000, Tint −100…100, luminance kept), then a color
removed (filter) and a color added (light) — `out = balance(in) × (1 − removed) + added` — edited in RGB, HSL,
additive, subtractive or all together. Every color parameter has its own switch, plus one for the whole section:
a switch off keeps its value and simply stops applying it, which makes a before / after easy. The whole section can
go through a **mask**, chosen at its top like an effect's: another layer's picture says where the color applies.
**Spatial** gives position (px) and scale (%, X and Y linked by default) of the mapped layer.

**Mapping.** 4 corners with true perspective (homography) and a deformation mesh from 2×2 to 32×32 points with
Catmull-Rom interpolation; the two combine — align the corners first, refine with the mesh. Points are
multi-selected and moved together. The preview zooms (wheel / − + Fit) for finer moves. Selected points show
their position in composition pixels, and the one clicked last can be typed in (bottom left of the preview; the
other selected points follow it). **Magnetism** (magnet button next to the zoom, on by default — Settings): a
dragged point is caught by the corners, edges and centers of the composition, the viewports, the other layers and
the layer's own still points; a dragged layer or viewport by its edges or its center; the bars by their notable
values. Guides show what caught it; ⌘ held while dragging moves freely.

**Effects.** An ISF chain per layer: any number of effects, reorderable, each one switchable, plus a master
switch for the chain. Each effect can take a **mask**: the picture of another layer (hidden or not) says where it
applies — fully on white, not at all on black or transparent, in proportion in between, or the reverse with
*Invert*. The mask is stretched over the layer's picture, before the mapping. Parameters are generated from each
shader's JSON header. **ISF v2** is supported (multiple
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
**Master** tab drives every viewport: level fader, Blackout with fade (⌘B, fades the sound too), audio output,
composition size and **rendering** — frame rate (the screen's refresh by default: the outputs' vertical sync, or
the main screen's rate), **antialiasing** of the mapped edges (2× / 4× / 8× MSAA) **color depth** (8 or 10 bits: the picture is mixed in 16-bit targets, which avoids banding after color and effects; the outputs stay 8 bits) and **mipmaps** for pictures
drawn much smaller than they are — each one the project's own or the machine's default (Settings ▸ Rendering). Syphon and Spout are built in; NDI and OMT are loaded only when enabled, so Fulskrin runs
without them.

**Media Bin.** Every media used by the project, grouped by type and searchable, with resolution, duration, sound
format and how many layers use it. Missing files are shown in red and **Replace…** relinks them everywhere at
once. An **ISF › Generators** category lists the library's generators, filed by ISF category. Drag an item onto a
layer to load it, or right-click it and pick the layer from **Load into Layer**.

**Memories** (the scenes / cues of MadMapper). A list — number, name, fade — of states of the layers: **+**
stores the current one, double-click, Enter or GO recalls, and a memory is dragged from there onto a step of a
sequence. Next to it, what the selected memory holds: its picture, name and fade, and its layers, each unfolding
into its values — source, ROI, color, mapping, effects and ISF parameters. Click a value: on the right, it can be
changed (the memory changes, the composition does not) and given how it gets there at the recall: **CUT** (at
once), **FOLLOW** (the memory's fade) or **a time of its own** — one time for the whole ROI, the whole mapping,
each color control and each ISF parameter. When a memory gives a layer **another source**, the outgoing one keeps
playing, invisible, and an **ISF transition** takes it to the new one over the source's time (the memory's fade, a
cut, or its own), before the layer's mapping; the sound crosses too, and the ROI, mapping and color move from the
outgoing one's. The transition is chosen per layer (Source tab), or by default in **Settings** (Crossfade);
Crossfade, Dissolve, Fade Out In, Iris and Wipe are bundled, and any ISF transition (`startImage`, `endImage`,
`progress`) of the library works. Effect chains switch at once; a layer deleted since is recreated; a layer the
memory does not know (created since) fades out and is hidden, so the picture is the one that was stored;
unchecked and locked layers are left alone.

**Sequences** (the cue list of the show). Ordered steps, each recalling a memory and carrying a text for the
operator; **Space** plays the next step (GO), **Shift+Space** the previous one (GO BACK). Under the preview, a bar
shows GO BACK, GO, the previous, current and next steps — the current one in another color once something was
changed since it was played — and the current step's text. **Sequences…** opens a window that stays in front:
several sequences (new, duplicate, delete, rename, loop — GO on the last step plays the first), their steps (+,
delete, move, a memory dragged from the list onto a step or below the last one, its text, double-click a number to
play it). Saved with the project; OSC `/sequence/go`, `/sequence/back`, `/sequence/step`, `/sequence/current`.

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
| GO: next step of the sequence · GO BACK | Space · ⇧Space |
| In / out points at the position | I / O |
| Every viewport fullscreen · in windows (again: back) | ⌘F · ⌘⇧F |
| Blackout (picture and sound) | ⌘B |
| Close the output from the output | ⇧Esc (Esc alone does nothing, for safety) |
| Undo / redo | ⌘Z / ⌘⇧Z |
| Save · discard on quit | ⌘S · ⌘D |
| Zoom · pan the preview | wheel or pinch, − + Fit · middle button or ⌥+drag |
| Select mapping points | ⌘+click, ⌘+drag a rectangle, ⌘A |
| Move points / layer | drag (⇧: fine, ⌘: without magnetism), or arrow keys (⇧: 10 px) |
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
| `…/color/mask` · `…/color/maskInvert` | s · T | mask of the color: a layer's name ("" for none) |
| `…/spatial/position` · `scale` · `corners/tl` `tr` `br` `bl` | ff | px · % · normalized |
| `…/effects/enabled` · `effects/<effect>/enabled` · `effects/<effect>/<input>` | | effect chain |
| `…/effects/<effect>/mask` · `maskInvert` | s · T | mask layer by name (`""`: none), inverted |
| `/layers/<group>/layers/<name>/…` | | layers of a group (and so on, for groups inside groups) |
| `/layers/<name>/viewports/<viewport>` | f | how much of the item that viewport shows, 0–1 (items at the top of the list) |
| `/layers/<name>/spatial/softedge/enabled` · `…/<left\|right\|top\|bottom>/width` · `power` | T · f | soft edge: fade towards each side (width 0–0.5 of the layer, power 0.1–8) |
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
  VideoDecoder  FFmpeg decoding on a thread (hardware when it can), frame queue, seamless loop, seeking;
                frames kept as decoded and copied into the GPU's upload buffers
  VideoTexture  a video layer's frames on the GPU: planes or compressed textures, converted to RGBA by one draw
  Hap, Snappy   HAP frames (sections, chunks, Snappy), blocks decoded on the CPU when the GPU cannot sample them
  WorkerPool    threads shared by the decoders (chunks of a frame side by side)
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
cmake --build build --target fulskrin_bench
./build/fulskrin_bench a.mov b.mov c.mov d.mov     # several videos played together, 4K composition
./build/fulskrin_bench --decode a.mov b.mov        # the decoders alone
```

`fulskrin_bench` plays every file in a layer of one composition for 10 s and tells, for each, how many frames per
second it showed against its own rate (`--seconds`, `--fps`, `--comp 3840x2160`).

`fulskrin_tests --check-isf <folder>` compiles and renders every shader in a folder.

## Known V1 limitations

- Hardware-decoded frames are copied back to memory before their upload (no zero-copy IOSurface / D3D11 sharing
  yet). Hidden layers keep decoding, so that showing them is instant: they cost processor time, not GPU time.
- **Sound**: one stereo output; no multichannel routing, per-layer output, fades or audio effects yet.
- **Viewports are rectangles** of the composition: no per-projector warp or automatic edge blending yet
  (the SoftEdges effect on a viewport helps with manual blending).
- No timeline, MIDI or DMX control; no camera, Syphon / Spout / NDI / OMT **inputs**.
- NDI and OMT send each viewport read back from the GPU (one frame of latency, noticeable CPU load in 4K).
  On Linux, OMT needs `avahi-daemon`, without which Fulskrin refuses to enable it. Syphon is OpenGL-only.
- ISF audio inputs are not supported (black texture).
- Transport and composition size are deliberately not undoable; undoing an effect-chain change reloads the
  layer's effects (Trails buffers restart from zero).
