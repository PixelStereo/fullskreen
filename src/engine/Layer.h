#pragma once
#include "AudioStream.h"
#include "Gl.h"
#include "Isf.h"
#include "Mapping.h"
#include "VideoDecoder.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

enum class SourceType { None, Video, Image, Isf, Audio };
enum class BlendMode { Normal, Add, Screen, Multiply };
// What a video or a sound does at its end: freeze on the last frame, loop, play backwards and forwards,
// or stop and go black (and silent).
enum class PlayMode { OneShot, Loop, PingPong, Stop };

QString playModeName(PlayMode m);
QString playModeKey(PlayMode m);
PlayMode playModeFromKey(const QString &k, PlayMode fallback = PlayMode::Loop);

QString blendModeName(BlendMode m);
QString blendModeKey(BlendMode m);
BlendMode blendModeFromKey(const QString &k);

// Color tab. First the balance, as in DaVinci Resolve: temperature (blue -4000 .. yellow +4000) and
// tint (green -100 .. magenta +100), luminance kept; then a color removed (filter) and a color added (light), RGB 0..1:
// out = balance(in) * (1 - remove) + add
struct ColorAdjust {
    float temp = 0, tint = 0;
    float add[3] = {0, 0, 0};
    float remove[3] = {0, 0, 0};
    static constexpr float kTempRange = 4000, kTintRange = 100;
    // Gains of the balance (1, 1, 1 when neutral)
    void balanceGains(float g[3]) const
    {
        const float t = temp / kTempRange, m = tint / kTintRange;
        g[0] = (1 + 0.4f * t) * (1 + 0.25f * m);
        g[1] = 1 - 0.3f * m;
        g[2] = (1 - 0.4f * t) * (1 + 0.25f * m);
        const float luma = 0.2126f * g[0] + 0.7152f * g[1] + 0.0722f * g[2];
        for (int c = 0; c < 3; ++c) g[c] = luma > 1e-6f ? g[c] / luma : 1.0f;
    }
    bool isIdentity() const
    {
        if (temp != 0.0f || tint != 0.0f) return false;
        for (int c = 0; c < 3; ++c)
            if (add[c] != 0.0f || remove[c] != 0.0f) return false;
        return true;
    }
    bool operator==(const ColorAdjust &o) const
    {
        if (temp != o.temp || tint != o.tint) return false;
        for (int c = 0; c < 3; ++c)
            if (add[c] != o.add[c] || remove[c] != o.remove[c]) return false;
        return true;
    }
    bool operator!=(const ColorAdjust &o) const { return !(*this == o); }
};

struct Layer {
    // Identity and structure. A group is a layer without source whose picture is the composite of its members;
    // members immediately follow their group in the layer list (one level: no group inside a group).
    quint64 id = 0;     // unique in the composition, saved in the project
    quint64 parent = 0; // id of the group containing the layer (0: top level)
    bool isGroup = false;
    bool collapsed = false; // group folded in the layer list
    int colorModels = 1;    // models shown by the Color tab for this layer (interface state, saved)

    QString name;
    bool visible = true;
    bool locked = false;          // no edit allowed (the visibility and the transport stay available)
    bool parentVisible = true;    // the group containing the layer is visible (updated every frame)
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;

    QRectF crop{0, 0, 1, 1};      // part of the source picture used (normalized, origin top left)
    ColorAdjust color;
    bool effectsEnabled = true;   // general switch of the effect chain

    SourceType type = SourceType::None;
    QString sourcePath;
    QString error;
    // File missing on load: path and type are kept (save, media bin, relink)
    SourceType missingType = SourceType::None;

    // Video and audio: transport shared by the picture and the sound
    std::unique_ptr<VideoDecoder> video;
    std::vector<uint8_t> frameBuffer;
    bool playing = true;
    PlayMode mode = PlayMode::Loop;
    bool ended = false; // Stop mode: the end was reached, the layer shows nothing
    // Transport: the clock advances with |speed| from a reposition (load, seek, direction or mode change);
    // the Timeline turns it into a position (legs forwards / backwards). Negative speed plays backwards.
    double speed = 1.0;
    double clock = 0.0, origin = 0.0;
    int dir = 1;
    uint64_t timelineId = 1; // incremented at every reposition (the sound resynchronizes)
    double inPoint = 0, outPoint = -1; // played range (out < 0: end of the media)

    // Sound: audio layer, or audio track of a video layer (null if the file has none)
    std::shared_ptr<AudioStream> audio;
    float volume = 1.0f; // linear gain, 0..2
    bool muted = false;

    // Video / image: source texture (fed in the render thread)
    Texture2D sourceTex;
    QImage pendingImage; // image to upload to the GPU on the next frame
    int srcWidth = 0, srcHeight = 0;

    // ISF generator
    std::unique_ptr<IsfInstance> generator;
    int genWidth = 1920, genHeight = 1080;
    RenderTarget generatorTarget;

    // ISF effect chain
    std::vector<std::unique_ptr<IsfInstance>> effects;
    RenderTarget fxTarget[2];

    RenderTarget groupTarget; // group: composite of its members (composition size)
    RenderTarget prepTarget;  // crop + color
    GLuint rawTex = 0;        // source picture before crop (crop preview)
    int rawW = 0, rawH = 0;

    Mapping mapping;

    // Frame render result
    GLuint finalTex = 0;
    int finalW = 0, finalH = 0;

    bool hasTransport() const { return video || audio; }
    double duration() const { return video ? video->duration() : audio ? audio->duration() : 0.0; }
    float audioGain() const { return visible && parentVisible && !muted && !ended ? volume : 0.0f; }
    bool repeats() const { return mode == PlayMode::Loop || mode == PlayMode::PingPong; }
    Timeline timeline() const
    {
        Timeline t;
        t.duration = duration();
        t.mode = mode == PlayMode::Loop ? Timeline::Loop : mode == PlayMode::PingPong ? Timeline::PingPong : Timeline::Once;
        t.origin = origin;
        t.dir = dir;
        t.in = inPoint;
        t.out = outPoint;
        return t;
    }
    double position() const { return timeline().position(clock); }
    bool atEnd() const { return timeline().ended(clock); } // One-shot / Stop: played to the end
    int sourceWidth() const { return type == SourceType::Isf ? genWidth : srcWidth; }
    int sourceHeight() const { return type == SourceType::Isf ? genHeight : srcHeight; }
    bool hasPicture() const { return isGroup || (type != SourceType::Audio && !(type == SourceType::None && missingType == SourceType::Audio)); }
    static QRectF fullCrop() { return QRectF(0, 0, 1, 1); }
};
