# Fulskrin — multi-layer video mapping (V1)

Video mapping application for stage and installation work, on Mac / Windows / Linux.
**Qt 6** interface, **FFmpeg** decoding, **OpenGL 3.3** rendering, effects and generators in the **ISF** format.

## What V1 does

- **Stacked layers** (the top layer is drawn on top), opacity, blend modes Normal / Add / Screen / Multiply.
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
  **Preferences** (Loop by default).
- **ISF effect chain per layer**: any number of effects, reorderable, each one can be enabled or disabled.
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
  Blackout does not affect the sound.
- **Master tab** (next to the Layer tab): master level fader and **Blackout** button with fade
  (adjustable duration, Ctrl+B / ⌘B, works from the output window too), output screen and mode,
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
- **Media Bin** (left): every video, image and audio file used by the project, grouped by type, with resolution,
  duration, sound format and the number of layers using them (sources and shader images). **Missing** files are shown in red;
  **Replace…** relinks them (a single replacement fixes every layer, and can be undone).
  An **ISF › Generators** category lists the generators of the ISF library (and those used by layers).
  Drag an item onto a layer to load it; double-click loads it into the selected layer.
- **Layers are created empty** with **+**, then loaded with whatever is dropped onto them: a video, an image,
  a sound or an ISF generator, from the Media Bin or the Finder / Explorer — onto the layer's row in the list,
  or onto the drop zone of its Source tab. An ISF effect dropped onto a layer joins its effect chain.
  The **×** next to the drop zone ejects the media.
- **Layer list at the bottom**, full width: visibility, name, source, effects, opacity adjustable
  directly in the row, blend mode, playback position.
- **Layer tab** with sub-tabs: **Source** (drop zone, transport, sound, generator parameters),
  **Spatial** (mapping), **Effects** (ISF chain), **Compositing** (opacity, blend).
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
| Close the output from the output | Shift+Esc (Esc alone does nothing, for safety) |
| Match the composition to the projector | Master tab ▸ Composition ▸ "= output screen" |
| Publish via NDI, OMT, Syphon, Spout | Master tab ▸ Output Publishing |
| Play / pause the selected video or audio layer | Space |
| Play mode of a video or a sound | Source tab ▸ One-shot / Loop / Ping-pong / Stop |
| Default play mode for newly loaded media | Preferences (⌘, on Mac, Ctrl+, elsewhere) |
| Move a handle | drag (Shift: fine movement) |
| Select several points (mesh or corners) | Ctrl/⌘+click to add or remove, Ctrl/⌘+drag a rectangle, Ctrl/⌘+A for all |
| Move the selected points together | drag one of them, or arrow keys |
| Move the whole layer | drag inside the layer |
| Nudge by one pixel | arrow keys (Shift: 10 px) |
| Next handle / deselect | Tab / Esc |
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

## Architecture

```
src/engine/   engine, with no widget dependency (QtCore/QtGui/OpenGL + FFmpeg)
  Engine        composition, render thread, output presentation, JSON project, Media Bin media
  Publish       publishing: NDI / OMT (loaded at runtime), Syphon (.mm), Spout; asynchronous GPU readback
  Isf           ISF parser and renderer (GLSL 330 core translation, passes, buffers)
  VideoDecoder  FFmpeg decoding on a thread, frame queue, seamless loop, seeking
  AudioStream   FFmpeg audio decoding + resampling on a thread, synchronized to the layer playhead
  AudioOutput   sound card (miniaudio), mix of all layers, master volume, meters
  Mapping       4-corner homography + Catmull-Rom mesh
src/ui/       Qt Widgets interface
  MainWindow, LayerInspector, ParamPanel, MappingView (editing), OutputWindow (projector)
  Commands      undo commands (QUndoStack)
  MediaBin, LayerTable, MasterPanel   Media Bin, layer list, Master tab
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
- **No timeline, cues, or OSC / MIDI / DMX control.**
- **No Syphon / Spout / NDI / OMT inputs, and no camera input** (the output can be published, though).
- NDI and OMT send the image at composition size, read back from the GPU (one frame of latency);
  for a 4K composition expect a noticeable CPU load, especially with OMT, which encodes the image.
- On Linux, OMT needs the Avahi service (`avahi-daemon`) for discovery; Fulskrin refuses to enable it without Avahi
  (the library would abort the program).
- Syphon is built OpenGL-only (no Metal server).
- ISF audio inputs are not supported (black texture).
- Video transport (play, pause, position) is deliberately not undoable; neither is the composition size.
- Undoing a change to the effect chain reloads the layer's effects (Trails buffers restart from zero).
