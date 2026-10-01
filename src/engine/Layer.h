#pragma once
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

enum class SourceType { None, Video, Image, Isf };
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

    // Vidéo
    std::unique_ptr<VideoDecoder> video;
    std::vector<uint8_t> frameBuffer;
    bool playing = true, loop = true;
    double speed = 1.0, playhead = 0.0;

    // Vidéo / image : texture source (alimentée dans le fil de rendu)
    Texture2D sourceTex;
    QImage pendingImage; // image à envoyer au GPU à la prochaine frame
    int srcWidth = 0, srcHeight = 0;

    // Générateur ISF
    std::unique_ptr<IsfInstance> generator;
    int genWidth = 1920, genHeight = 1080;
    RenderTarget generatorTarget;

    // Chaîne d'effets ISF
    std::vector<std::unique_ptr<IsfInstance>> effects;
    RenderTarget fxTarget[2];

    Mapping mapping;

    // Résultat du rendu de la frame
    GLuint finalTex = 0;
    int finalW = 0, finalH = 0;

    double duration() const { return video ? video->duration() : 0.0; }
    double position() const
    {
        const double d = duration();
        if (d <= 0) return playhead;
        return loop ? std::fmod(playhead, d) : std::min(playhead, d);
    }
    int sourceWidth() const { return type == SourceType::Isf ? genWidth : srcWidth; }
    int sourceHeight() const { return type == SourceType::Isf ? genHeight : srcHeight; }
};
