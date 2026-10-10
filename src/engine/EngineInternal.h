#pragma once
// Shared by the Engine translation units (Engine.cpp, Layers.cpp, Render.cpp, Project.cpp, Media.cpp,
// Snapshots.cpp): the holder of detached resources, and the few helpers more than one of them needs.
// Nothing here is part of the engine's API — Engine.h is.

#include "Engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>

inline constexpr int kMeshSubdiv = 40;

// Resources removed from the composition (under lock), then released in the render thread.
// The Text generator's settings in a layer state (Project.cpp)
// An easing of the snapshots' fades (0 linear, 1 in, 2 out, 3 in-out, 4 in cubic, 5 out cubic) at t in 0..1
double easeCurve(double t, int curve);
// Keys of a timeline's loop mode in a project ("once", "loop", "pingpong")
QString animLoopKey(Engine::AnimLoop l);
Engine::AnimLoop animLoopFromKey(const QString &k);
// Keys of the waves ("sine", "triangle", "saw", "square", "random", "smooth_random")
QStringList animWaveKeys();
inline QString animWaveKey(AnimWave w) { return animWaveKeys().value(int(w)); }
// A layer's animation of one of its numbers in its layer state ("anims"): its number, transport, keys, oscillator
QJsonObject layerAnimToJson(const Animation &a);
Animation layerAnimFromJson(const QJsonObject &o, quint64 layer);

struct Engine::Garbage {
    std::unique_ptr<VideoDecoder> video;
    std::unique_ptr<VideoTexture> videoTex;
    std::shared_ptr<AudioStream> audio;
    Texture2D tex, textTex;
    std::unique_ptr<IsfInstance> generator;
    RenderTarget generatorTarget;
    std::vector<std::unique_ptr<IsfInstance>> effects;
    std::unique_ptr<Layer> layer;
};

// The stream leaves the mix before its decode thread stops.
inline void releaseAudio(AudioOutput &out, std::shared_ptr<AudioStream> &a)
{
    if (!a) return;
    out.removeStream(a.get());
    a->close();
    a.reset();
}

// Restarts the layer's clock at `position`, in direction `dir`: decoders follow the new timeline.
inline void reposition(Layer &l, double position, int dir)
{
    const double d = l.duration();
    const Timeline t = l.timeline();
    l.origin = d > 0 ? std::clamp(position, t.lo(), t.hi()) : std::max(0.0, position);
    l.dir = dir >= 0 ? 1 : -1;
    l.clock = 0;
    l.ended = false;
    ++l.timelineId;
    if (l.video) {
        l.video->setTimeline(l.timeline());
        l.video->seek(0);
    }
    if (l.audio) l.audio->setTransport(0, l.playing, std::abs(l.speed) * timeScale(), l.timeline(), l.timelineId, l.audioGain());
}

// A media starts at its in point — its out point when the speed is negative.
inline void startMedia(Layer &l)
{
    l.dir = l.speed < 0 ? -1 : 1;
    const Timeline t = l.timeline();
    reposition(l, l.dir < 0 ? t.hi() : t.lo(), l.dir);
}

// ROI of a saved layer object (the whole picture when it is absent or degenerate)
