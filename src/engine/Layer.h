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
    bool playing = true, loop = true;
    double speed = 1.0, playhead = 0.0;

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
    float audioGain() const { return visible && !muted ? volume : 0.0f; }
    double position() const
    {
        const double d = duration();
        if (d <= 0) return playhead;
        return loop ? std::fmod(playhead, d) : std::min(playhead, d);
    }
    int sourceWidth() const { return type == SourceType::Isf ? genWidth : srcWidth; }
    int sourceHeight() const { return type == SourceType::Isf ? genHeight : srcHeight; }
};
