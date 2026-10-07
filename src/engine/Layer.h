#pragma once
#include "AudioStream.h"
#include "Gl.h"
#include "Isf.h"
#include "Mapping.h"
#include "Publish.h"
#include "VideoDecoder.h"
#include "VideoTexture.h"

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

// What feeds a layer's picture. `Layer`: the picture of another layer of the composition, tapped either
// before or after its effect chain (see LayerTap) — the same picture can be mapped and treated twice.
// `Text`: rendered text layer with system fonts, word wrapping, and animation.
enum class SourceType { None, Video, Image, Isf, Audio, Layer, Text };

// The Text generator is built in: the Media Bin lists it under Generators with this (virtual) path, so that it is
// dragged onto a layer like any media.
inline const QString kTextGeneratorPath = QStringLiteral("/Fulskrin/Generators/Text");
inline bool isTextGeneratorPath(const QString &p) { return QString(p).replace('\\', '/').endsWith(kTextGeneratorPath); }
// Where the picture of a layer used as a source is taken
enum class LayerTap {
    PreFx = 0,  // after its ROI and color, before its effect chain
    PostFx = 1, // after its effect chain (its own mapping and opacity are not part of it)
};
QString layerTapKey(LayerTap t);
LayerTap layerTapFromKey(const QString &k);
enum class BlendMode { Normal, Add, Screen, Multiply, Subtract, Difference };
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
    // Switches: the whole color section, then one per parameter. A parameter switched off keeps its value
    // (it comes back as it was) but is not applied.
    bool enabled = true, tempOn = true, tintOn = true, addOn = true, removeOn = true;
    // Where the section applies: another layer's picture stretched over this one, fully on white, not at all on
    // black or transparent (0: everywhere). As the mask of an effect, but once for the whole section.
    quint64 maskLayer = 0;
    bool maskInvert = false;
    static constexpr float kTempRange = 4000, kTintRange = 100;

    // What the rendering applies: a parameter that is off is neutral
    ColorAdjust effective() const
    {
        ColorAdjust e = *this;
        if (!enabled || !tempOn) e.temp = 0;
        if (!enabled || !tintOn) e.tint = 0;
        for (int c = 0; c < 3; ++c) {
            if (!enabled || !addOn) e.add[c] = 0;
            if (!enabled || !removeOn) e.remove[c] = 0;
        }
        return e;
    }
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
        if (enabled != o.enabled || tempOn != o.tempOn || tintOn != o.tintOn || addOn != o.addOn ||
            removeOn != o.removeOn || maskLayer != o.maskLayer || maskInvert != o.maskInvert)
            return false;
        for (int c = 0; c < 3; ++c)
            if (add[c] != o.add[c] || remove[c] != o.remove[c]) return false;
        return true;
    }
    bool operator!=(const ColorAdjust &o) const { return !(*this == o); }
};

// The Text generator's settings: what is typed and how it is set. Plain data, copied by the snapshots' fades.
struct TextSource {
    QString content;                           // the text
    QString font = QStringLiteral("Arial");    // system font family
    int size = 48;                             // pixels
    QColor color = QColor(255, 255, 255);
    Qt::Alignment align = Qt::AlignHCenter | Qt::AlignVCenter; // left / center / right / justify | top / middle / bottom
    float lineHeight = 1.2f;                   // multiple of the font's line spacing
    float letterSpacing = 0.0f;                // pixels added between the letters
    bool bold = false, italic = false, underline = false, strike = false;
    float outline = 0.0f;                      // outline width in pixels (0: none)
    QColor outlineColor = QColor(0, 0, 0);
    bool shadow = false;                       // drop shadow, offset in pixels
    QColor shadowColor = QColor(0, 0, 0, 160);
    float shadowX = 4.0f, shadowY = 4.0f;
    int width = 1920, height = 1080;           // size of its picture

    // Typewriter: a snapshot that gives another text types it over its time. The shown text goes from `typedFrom` to
    // `content` (erasing back to what they share, then typing) as `typeProgress` goes from 0 to 1; not typing when
    // `typeDur` is 0.
    QString typedFrom;
    double typeElapsed = 0, typeDur = 0, typeProgress = 1;
    int typeCurve = 0; // easing of the typing (EasingCurve, Snapshots.cpp)

    QString shown() const
    {
        if (typeDur <= 0) return content;
        const QString &a = typedFrom, &b = content;
        int c = 0;
        while (c < a.size() && c < b.size() && a[c] == b[c]) ++c;
        const int del = int(a.size()) - c, total = del + int(b.size()) - c;
        const int steps = int(std::clamp(typeProgress, 0.0, 1.0) * total);
        return steps <= del ? a.left(int(a.size()) - steps) : b.left(c + steps - del);
    }
    void stopTyping() { typeDur = typeElapsed = 0; typeProgress = 1; typedFrom.clear(); }
    // Values kept in their useful range (a project or a snapshot read from a file may hold anything)
    void sanitize()
    {
        size = std::clamp(size, 1, 1000);
        lineHeight = std::clamp(lineHeight, 0.1f, 10.0f);
        letterSpacing = std::clamp(letterSpacing, -200.0f, 500.0f);
        outline = std::clamp(outline, 0.0f, 200.0f);
        shadowX = std::clamp(shadowX, -2000.0f, 2000.0f);
        shadowY = std::clamp(shadowY, -2000.0f, 2000.0f);
        width = std::clamp(width, 1, 16384);
        height = std::clamp(height, 1, 16384);
        if (font.isEmpty()) font = QStringLiteral("Arial");
    }
};

struct Layer {
    // Identity and structure. A group is a layer without source whose picture is the composite of its members;
    // members immediately follow their group in the layer list, and a group can hold other groups.
    // Layers and groups live in the composition: one pixel space, set in the Composition tab.
    // A viewport is a window onto that space with its own size in pixels, sent to a screen (and published).
    // It holds nothing: it shows the part of the composition its Spatial places it on, with its own ROI,
    // color, effects and opacity. Viewports come first in the list and are never inside a group.
    quint64 id = 0;     // unique in the composition, saved in the project
    quint64 parent = 0; // id of the group containing the layer (0: top level)
    bool isGroup = false;
    bool collapsed = false; // group folded in the layer list

    // Viewport
    bool isViewport = false;
    int vpWidth = 1920, vpHeight = 1080; // size of its picture, in pixels
    QString vpScreen;                    // screen it is shown on (empty: the main one)
    int vpMode = 0;                      // 0 hidden, 1 windowed, 2 fullscreen
    PublishSettings vpPublish;           // NDI, OMT, Syphon, Spout of this viewport
    // How much of this item each viewport shows (an item at the top of the list; inside a group, the group decides).
    // A viewport that is not in the map shows it fully, so a viewport created later shows everything.
    std::map<quint64, float> viewportOpacity;
    int colorModels = 1;    // models shown by the Color tab for this layer (interface state, saved)

    QString name;
    bool enabled = true; // off: the layer is not rendered, not heard, not shown
    bool locked = false;          // no edit allowed (the visibility and the transport stay available)
    bool parentEnabled = true;    // the group containing the layer is visible (updated every frame)
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;

    QRectF roi{0, 0, 1, 1};      // part of the source picture used (normalized, origin top left)
    ColorAdjust color;
    bool effectsEnabled = true;   // general switch of the effect chain

    SourceType type = SourceType::None;
    QString sourcePath;
    QString error;
    // SourceType::Layer: the layer whose picture is used, and where it is tapped
    quint64 sourceLayer = 0;
    LayerTap sourceTap = LayerTap::PostFx;
    bool referenced = false; // used as a source by another layer: rendered even when hidden (every frame)
    bool needed = true;      // its picture is drawn this frame (shown, or referenced): uploaded and rendered
    // File missing on load: path and type are kept (save, media bin, relink)
    SourceType missingType = SourceType::None;

    // Video and audio: transport shared by the picture and the sound
    std::unique_ptr<VideoDecoder> video;
    VideoFrame frame;                       // the last one fetched from the decoder
    bool frameUploaded = true;              // and already on the GPU
    std::unique_ptr<VideoTexture> videoTex; // its picture (created in the render thread)
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

    // Text generator (type Text): its settings, and the texture they are drawn into (render thread)
    TextSource text;
    Texture2D textTex;
    QString textKey; // what textTex shows (text and style): redrawn only when it changes

    // ISF generator
    std::unique_ptr<IsfInstance> generator;
    int genWidth = 1920, genHeight = 1080;
    RenderTarget generatorTarget;

    // ISF effect chain
    std::vector<std::unique_ptr<IsfInstance>> effects;
    RenderTarget fxTarget[2];

    RenderTarget groupTarget; // group: composite of its members; viewport: what it sees, at its own size
    // Viewport: the picture its window shows (composition opacity and blackout applied), double buffered so the
    // interface and the publishers read the last finished frame
    RenderTarget vpOut[2];
    int vpBack = 1;
    std::atomic<int> vpPublished{0};
    RenderTarget prepTarget;  // roi + color
    GLuint rawTex = 0;        // source picture before roi (roi preview)
    int rawW = 0, rawH = 0;

    Mapping mapping;
    GLuint meshVbo = 0;  // its mesh's vertices (render thread)
    Mapping meshShape;   // the mapping they were built from

    // Transition used when a snapshot gives this layer another source (ISF with startImage, endImage, progress;
    // empty: the default one, chosen in the settings)
    QString transition;
    float transitionGain = 1.0f; // sound during such a transition: the incoming source rises, the outgoing one falls

    RenderTarget maskTarget[2]; // an effect's result through its mask

    // Frame render result
    GLuint finalTex = 0;
    int finalW = 0, finalH = 0;
    GLuint preFxTex = 0; // picture just before the effect chain (tap of a layer used as a source)
    int preFxW = 0, preFxH = 0;

    bool hasTransport() const { return video || audio; }
    double duration() const { return video ? video->duration() : audio ? audio->duration() : 0.0; }
    float audioGain() const { return enabled && parentEnabled && !muted && !ended ? volume * transitionGain : 0.0f; }
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
    int sourceWidth() const { return type == SourceType::Isf ? genWidth : type == SourceType::Text ? text.width : srcWidth; }
    int sourceHeight() const { return type == SourceType::Isf ? genHeight : type == SourceType::Text ? text.height : srcHeight; }
    bool hasPicture() const { return isGroup || isViewport || (type != SourceType::Audio && !(type == SourceType::None && missingType == SourceType::Audio)); }
    bool isText() const { return type == SourceType::Text; }
    QSize viewportSize() const { return QSize(std::max(1, vpWidth), std::max(1, vpHeight)); }
    float opacityIn(quint64 viewport) const
    {
        const auto it = viewportOpacity.find(viewport);
        return it == viewportOpacity.end() ? 1.0f : it->second;
    }
    bool shownIn(quint64 viewport) const { return opacityIn(viewport) > 0.0f; }
    static QRectF fullRoi() { return QRectF(0, 0, 1, 1); }
};
