# Fulskrin

Professional video mapping for stage, installation, and live performance. Project, warp, and blend video across multiple outputs with frame-perfect playback, real-time effects, and OSC control.

Built with Qt 6, FFmpeg, and OpenGL 3.3. Cross-platform: macOS, Windows, Linux.

## What You Get

**Spatial control** — 4-corner perspective warping + deformable mesh (up to 32×32 points) for projection onto complex surfaces.

**Composition** — Unlimited layers with opacity, blend modes (Normal, Add, Screen, Multiply), and per-viewport routing. Group layers and nest them.

**Sources** — Video (any FFmpeg codec, with sound), stills, live ISF generators, audio files, or other layers (pre- or post-FX).

**Color** — Temperature, tint, and RGB/HSL controls. Add light, remove color, all toggleable and reversible.

**Effects** — ISF 1.3+ shader chain per layer. Includes generators (TestPattern, Plasma, Gradient, Clouds), color correction, blur, trails, kaleidoscope, soft edges. Load your own from `~/Library/Graphics/ISF` or bundled folder.

**Snapshot system** — Save and recall layer states (cues). Every parameter fades smoothly to its stored value over a configurable duration. Undo/redo throughout.

**Sequences** — Cue lists of snapshots played with GO (button, Space, OSC). Each step has a pre-wait, a post-wait and a continuation, as in QLab: *Wait* (the next step waits for GO), *Follow* (the next step goes once this one is triggered, plus its post-wait), *Auto-follow* (the next step goes once this one's fade is over, plus its post-wait). The waits and fades are drawn filling up as they run. Snapshots run side by side: a snapshot recalled takes over only the values it holds, the others keep fading. A step recalls a snapshot or drives a timeline: drag either onto it.

**Timelines** — Numbers of the layers (and of the composition) animated over time, outside the sequences: opacity, volume, speed, ROI, color, spatial position, scale and rotation, text settings, every ISF parameter. Each track is a *curve* (keys clicked, dragged or drawn freehand, eased from one to the next or held) or an *oscillator* (sine, triangle, saw, square; period, phase, center, amplitude). A timeline has a duration and loops once, endlessly or a number of times, forwards or ping-pong; the curves repeat their pattern, the oscillators run on without a jump. Several play side by side. The sequences drive their transport — a timeline dropped on a step plays it, and the step can pause, stop, rewind, seek or change its loop instead — so a timeline can keep turning while the next snapshots come, and stop many steps later. While a timeline plays, its values win over the snapshots' fades.

**Sound** — Each layer plays its audio with volume, mute, and sync to playback (including speed). Mixed to stereo, meters included.

**Output** — Fullscreen or windowed. Synced to vertical refresh. Blackout with fade. Publish to Syphon (macOS), Spout (Windows), NDI, or OMT.

**Control** — Full OSC/OSCQuery support. Script or trigger everything from external tools (Vezér, Chataigne, score, etc.). Zeroconf announcement.

## Quick Start

### macOS

```bash
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
open build/Fulskrin.app
```

Distribute with `macdeployqt`. Not signed—right-click → Open on first launch.

### Windows 10/11

Install Qt 6 (MSVC 2022 64-bit) and Visual Studio 2022. Download shared FFmpeg with dev files.

```bat
cmake -S . -B build ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64 ^
  -DFFMPEG_ROOT=C:\ffmpeg-8.1-shared
cmake --build build --config Release
windeployqt.exe build\Release\Fulskrin.exe
```

### Linux (Ubuntu/Debian)

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev \
  libgl1-mesa-dev libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev
cmake -S . -B build && cmake --build build -j
./build/Fulskrin
```

## Technical Details

**Requirements:**
- Qt 6.2+
- FFmpeg 4.0+ (libavformat, libavcodec, libswscale, libswresample, libavutil)
- CMake 3.21+
- C++17 compiler
- OpenGL 3.3+ (Core Profile)
- miniaudio (downloaded automatically)

**Rendering Pipeline:**
- Dedicated OpenGL thread with vertical sync
- GPU → interface live preview
- Per-layer: ROI crop, color balance, FX chain (ISF), spatial transform
- Per-viewport: opacity + routing (which outputs show this layer)
- Per-output: homography + mesh warp + soft edge feathering (per-side width & power)
- Final composite: blend modes, composition opacity, publishing (Syphon/Spout/NDI/OMT)

**Audio:**
- FFmpeg decode + resampling on dedicated thread
- Synced to layer playhead (play, pause, seek, speed, loop)
- Per-layer: volume (0–200%), mute, meter
- Summed to 48 kHz stereo, sent to system audio output

**ISF Support:**
- ISF v1.3+, standard and extended syntax
- Multi-pass rendering, computed pass sizes, persistent/float buffers, imported images
- Vertex shaders (`.vs`), custom events
- Tested against Vidvox official library (256/259 generators & effects work)

**Project Format:**
- JSON (`.fulskrin`), human-readable
- Paths relative to project (portable between machines)
- Atomic writes, autosave every 10 seconds
- Undo/redo for everything except transport and canvas size

**OSC:**
- UDP 1234 (OSC), TCP 5678 (OSCQuery HTTP + WebSocket)
- Zeroconf announce (`_oscjson._tcp`, `_osc._udp`)
- Full namespace remote control
- Timelines: `/timelines/<n>/play`, `pause`, `stop`, `rewind`, `seek <seconds>`, `playing`
- Address patterns and bundles supported

## Known Limitations

**Performance:**
- CPU RGBA decoding is fine for a few HD streams. 4K or multiple streams will benefit from GPU decode (VideoToolbox on macOS, D3D11VA on Windows, VA-API on Linux) and hardware HAP decompression.

**Audio:**
- Single stereo output only; no multichannel routing, per-layer aux sends, or audio effects yet.

**Output:**
- One output window. No per-projector edge blending automation (the SoftEdges effect enables manual blending).

**Features not in V1:**
- MIDI or DMX input
- Camera, texture, or network inputs (Syphon/Spout/NDI/OMT receive)
- Nested groups
- ISF audio inputs (treated as black)
- Transitions between snapshots

**Publishing:**
- NDI and OMT send a GPU readback (one frame latency, CPU-intensive in 4K)
- OMT on Linux requires `avahi-daemon`
- Syphon is OpenGL-only (macOS)

## Links

- **ISF specification:** https://www.interactiveshaderformat.com/
- **OSCQuery:** https://github.com/Vidvox/OSCQueryProposal
- **Bundled shaders:** `isf/` folder after build

---

Built for video artists, theatre, and installation work. Made with care.
