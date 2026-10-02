#pragma once
#include "AudioStream.h"
#include "Gl.h"
#include "Isf.h"
#include "Mapping.h"
#include "VideoDecoder.h"

#include <QImage>
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

struct Layer {
    QString name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;

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

    Mapping mapping;

    // Frame render result
    GLuint finalTex = 0;
    int finalW = 0, finalH = 0;

    bool hasTransport() const { return video || audio; }
    double duration() const { return video ? video->duration() : audio ? audio->duration() : 0.0; }
    float audioGain() const { return visible && !muted && !ended ? volume : 0.0f; }
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
};
