#pragma once
// Shared by the Engine translation units (Engine.cpp, Layers.cpp, Render.cpp, Project.cpp, Media.cpp,
// Memories.cpp): the holder of detached resources, and the few helpers more than one of them needs.
// Nothing here is part of the engine's API — Engine.h is.

#include "Engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>

inline constexpr int kMeshSubdiv = 40;

// Resources removed from the composition (under lock), then released in the render thread.
// The Text generator's settings in a layer state (Project.cpp)
QJsonObject textJson(const TextSource &t);
void readTextJson(TextSource &t, const QJsonObject &o); // onto the current values, then kept in range
// An easing of the memories' fades (0 linear, 1 in, 2 out, 3 in-out, 4 in cubic, 5 out cubic) at t in 0..1
double easeCurve(double t, int curve);
// Keys of a timeline's loop mode in a project ("once", "loop", "pingpong")
QString animLoopKey(Engine::AnimLoop l);
Engine::AnimLoop animLoopFromKey(const QString &k);

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
    if (l.audio) l.audio->setTransport(0, l.playing, std::abs(l.speed), l.timeline(), l.timelineId, l.audioGain());
}

// A media starts at its in point — its out point when the speed is negative.
inline void startMedia(Layer &l)
{
    l.dir = l.speed < 0 ? -1 : 1;
    const Timeline t = l.timeline();
    reposition(l, l.dir < 0 ? t.hi() : t.lo(), l.dir);
}

// ROI of a saved source object (the whole picture when it is absent or degenerate)
inline QRectF roiFromJson(const QJsonObject &src)
{
    const QJsonArray c = src.value("roi").toArray();
    if (c.size() != 4) return Layer::fullRoi();
    const QRectF r = QRectF(QPointF(c[0].toDouble(), c[1].toDouble()), QPointF(c[2].toDouble(), c[3].toDouble()))
                         .normalized() & Layer::fullRoi();
    return r.isEmpty() ? Layer::fullRoi() : r;
}

inline void colorFromJson(ColorAdjust &col, const QJsonObject &o)
{
    col.temp = float(std::clamp(o.value("temp").toDouble(0), -double(ColorAdjust::kTempRange), double(ColorAdjust::kTempRange)));
    col.tint = float(std::clamp(o.value("tint").toDouble(0), -double(ColorAdjust::kTintRange), double(ColorAdjust::kTintRange)));
    for (int c = 0; c < 3; ++c) {
        col.add[c] = float(std::clamp(o.value("add").toArray().at(c).toDouble(0), 0.0, 1.0));
        col.remove[c] = float(std::clamp(o.value("remove").toArray().at(c).toDouble(0), 0.0, 1.0));
    }
    // Switches: on when absent
    col.enabled = o.value("enable").toBool(true);
    col.tempOn = o.value("temp_enable").toBool(true);
    col.tintOn = o.value("tint_enable").toBool(true);
    col.addOn = o.value("add_enable").toBool(true);
    col.removeOn = o.value("remove_enable").toBool(true);
    col.maskLayer = o.value("mask").toString().toULongLong(); // ids are strings: JSON numbers are doubles
    col.maskInvert = o.value("mask_invert").toBool(false);
}
