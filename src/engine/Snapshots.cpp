// Snapshots (cues): snapshots of the layers, recalled with a fade; another source comes in with a transition.
#include "EngineInternal.h"
#include "Params.h"
#include "Osc.h"

#include <QBuffer>
#include <QDir>
#include <QJsonArray>
#include <QSet>

// Easing curve types for parameter interpolation
enum class EasingCurve { Linear, EaseIn, EaseOut, EaseInOut, EaseInCubic, EaseOutCubic };

QStringList Engine::easingKeys()
{
    return {QStringLiteral("linear"),      QStringLiteral("ease_in"),       QStringLiteral("ease_out"),
            QStringLiteral("ease_in_out"), QStringLiteral("ease_in_cubic"), QStringLiteral("ease_out_cubic")};
}

QStringList Engine::easingNames()
{
    return {QStringLiteral("Linear"),      QStringLiteral("Ease In"),       QStringLiteral("Ease Out"),
            QStringLiteral("Ease In-Out"), QStringLiteral("Ease In Cubic"), QStringLiteral("Ease Out Cubic")};
}

QString Engine::defaultEasing(const QString &timingKey)
{
    return timingKey == QLatin1String("source/text/content") ? QStringLiteral("linear") : QStringLiteral("ease_in_out");
}

EasingCurve easingCurveFromKey(const QString &k)
{
    const int i = Engine::easingKeys().indexOf(k);
    return i < 0 ? EasingCurve::EaseInOut : EasingCurve(i); // in-out by default
}

// Easing function: applies curve type to normalized time [0..1]
static double applyEasing(double t, EasingCurve curve)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (curve) {
    case EasingCurve::Linear: return t;
    case EasingCurve::EaseIn: return t * t; // quadratic
    case EasingCurve::EaseOut: return t * (2 - t);
    case EasingCurve::EaseInOut: return t * t * (3 - 2 * t); // smoothstep
    case EasingCurve::EaseInCubic: return t * t * t;
    case EasingCurve::EaseOutCubic: return 1 - (1 - t) * (1 - t) * (1 - t);
    }
    return t;
}

double easeCurve(double t, int curve) { return applyEasing(t, EasingCurve(std::clamp(curve, 0, 5))); }

// The Text generator's values that fade, with their timing keys (their OSC address; their path in the
// state is source/<key>). The content is "typed" over its time.
enum TextNum { TextSize, TextColor, TextLineHeight, TextLetterSpacing, TextOutline, TextOutlineColor, TextShadowColor,
               TextShadowX, TextShadowY, TextContent, TextNumCount };
static const char *const kTextNumKeys[TextNumCount] = {"size",         "color",         "line_height", "letter_spacing",
                                                       "outline",      "outline_color",  "shadow_color", "shadow_x",
                                                       "shadow_y",      "content"};
// The same values as OSC addresses (timing keys)
static const char *const kTextTimeKeys[TextNumCount] = {"source/text/size",         "source/text/color",          "source/text/line_height",
                                                        "source/text/letter_spacing", "source/text/outline",        "source/text/outline/color",
                                                        "source/text/shadow/color", "source/text/shadow/x",      "source/text/shadow/y",
                                                        "source/text/content"};

struct TextNumbers {
    float size = 48, lineHeight = 1.2f, letterSpacing = 0, outline = 0, shadowX = 4, shadowY = 4;
    QColor color, outlineColor, shadowColor;
};
static TextNumbers textNumbersOf(const TextSource &t)
{
    TextNumbers n;
    n.size = float(t.size);
    n.lineHeight = t.lineHeight;
    n.letterSpacing = t.letterSpacing;
    n.outline = t.outline;
    n.shadowX = t.shadowX;
    n.shadowY = t.shadowY;
    n.color = t.color;
    n.outlineColor = t.outlineColor;
    n.shadowColor = t.shadowColor;
    return n;
}

// Numbers of a layer that fade from one snapshot to the next
struct LayerNumbers {
    float opacity = 1, volume = 1;
    QRectF roi;
    ColorAdjust color;
    Mapping mapping;
    std::vector<std::vector<IsfValue>> isf; // [0] generator, [1 + k] effect k
    std::map<quint64, float> viewportOpacity; // per-viewport opacity (0..1)
    SoftEdge soft; // crop feathering (width and power per side)
    double speed = 1.0; // the media's playback speed, or the generator's (TIME pace) on a layer without transport
    std::vector<double> fxSpeed; // TIME pace of each effect
    double inPoint = 0, outPoint = -1;
    TextNumbers text; // Text generator
};

// How long each of those numbers takes to reach the snapshot's value (seconds; 0: a cut). By default the
// snapshot's fade; a snapshot can give any of them a time of its own ("timing" in its layer state).
struct LayerTimes {
    double opacity = 0, volume = 0, roi = 0, temp = 0, tint = 0, add = 0, remove = 0, mapping = 0;
    double pivot = 0; // the pivot of the rotation (a cut unless the snapshot says otherwise)
    std::vector<std::vector<double>> isf; // as LayerNumbers::isf
    double viewportOpacity = 0; // viewport opacity per-viewport
    double softEdge = 0; // soft edge width and power
    double speed = 0, inPoint = 0, outPoint = 0; // playback parameters
    std::vector<double> fxSpeed; // as LayerNumbers::fxSpeed
    double text[TextNumCount] = {}; // Text generator (TextContent: the time the text is typed in)
    // Easing curves for each parameter (default: EaseInOut for all)
    EasingCurve opacityCurve = EasingCurve::EaseInOut, volumeCurve = EasingCurve::EaseInOut;
    EasingCurve roiCurve = EasingCurve::EaseInOut, colorCurve = EasingCurve::EaseInOut;
    EasingCurve mappingCurve = EasingCurve::EaseInOut, pivotCurve = EasingCurve::EaseInOut, softEdgeCurve = EasingCurve::EaseInOut;
    EasingCurve viewportOpacityCurve = EasingCurve::EaseInOut;
    EasingCurve speedCurve = EasingCurve::EaseInOut, inOutCurve = EasingCurve::EaseInOut;
    std::vector<EasingCurve> fxSpeedCurves;
    EasingCurve textCurve[TextNumCount] = {};
    std::vector<std::vector<EasingCurve>> isfCurves; // per-parameter curves

    double longest() const
    {
        double m = std::max({opacity, volume, roi, temp, tint, add, remove, mapping, pivot, viewportOpacity, softEdge, speed, inPoint, outPoint});
        for (double d : text) m = std::max(m, d);
        for (const auto &v : isf)
            for (double d : v) m = std::max(m, d);
        for (double d : fxSpeed) m = std::max(m, d);
        return m;
    }
};

// Which of a layer's values a fade drives. Several snapshots can run at once: a new recall takes over only
// the values it holds, the others go on with the snapshot that started them.
enum OwnedBit : quint64 {
    OwnOpacity = 1ull << 0, OwnVolume = 1ull << 1, OwnRoi = 1ull << 2, OwnColor = 1ull << 3, OwnMapping = 1ull << 4,
    OwnSoft = 1ull << 5, OwnViewportOpacity = 1ull << 6, OwnSpeed = 1ull << 7, OwnInOut = 1ull << 8, OwnIsf = 1ull << 9,
    OwnText0 = 1ull << 10, // + TextNum
};
static constexpr quint64 kOwnText = ((1ull << TextNumCount) - 1) << 10;
static constexpr quint64 kOwnAll = OwnOpacity | OwnVolume | OwnRoi | OwnColor | OwnMapping | OwnSoft | OwnViewportOpacity |
                                   OwnSpeed | OwnInOut | OwnIsf | kOwnText;

struct Engine::FadeJob {
    quint64 id = 0;
    LayerNumbers from, to;
    LayerTimes times;
    quint64 owned = kOwnAll; // the values this fade drives (a later recall takes some over)
    double elapsed = 0;      // its own clock: snapshots started at different times run side by side
    bool hideAtEnd = false;
    float finalOpacity = 1;
    // Numbers a timeline or a layer's animation has driven since it started (their addresses): it leaves them where
    // they are, so that one played after a snapshot ends where it ends, while the rest of the snapshot fades on
    QStringList released;
};

static LayerNumbers numbersOf(const Layer &l)
{
    LayerNumbers n;
    n.opacity = l.opacity;
    n.volume = l.volume;
    n.roi = l.roi;
    n.color = l.color;
    n.mapping = l.mapping;
    n.viewportOpacity = l.viewportOpacity;
    n.soft = l.mapping.soft;
    n.speed = !l.hasTransport() && l.generator ? l.generator->speed : l.speed;
    for (const auto &fx : l.effects) n.fxSpeed.push_back(fx->speed);
    n.inPoint = l.inPoint;
    n.outPoint = l.outPoint;
    n.text = textNumbersOf(l.text);
    auto values = [](const IsfInstance *inst) {
        std::vector<IsfValue> v;
        if (inst)
            for (const IsfInput &in : inst->inputs()) v.push_back(in.value());
        return v;
    };
    n.isf.push_back(values(l.generator.get()));
    for (const auto &fx : l.effects) n.isf.push_back(values(fx.get()));
    return n;
}

static void setNumbers(Layer &l, const LayerNumbers &n, quint64 own = kOwnAll)
{
    if (own & OwnOpacity) l.opacity = n.opacity;
    if (own & OwnVolume) l.volume = n.volume;
    if (own & OwnRoi) l.roi = n.roi;
    if (own & OwnColor) {
        // its numbers only: the switches and the mask are not faded
        l.color.temp = n.color.temp;
        l.color.tint = n.color.tint;
        for (int c = 0; c < 3; ++c) {
            l.color.add[c] = n.color.add[c];
            l.color.remove[c] = n.color.remove[c];
        }
    }
    if (own & (OwnMapping | OwnSoft)) {
        const unsigned rev = l.mapping.revision;
        const SoftEdge soft = l.mapping.soft;
        if (own & OwnMapping) l.mapping = n.mapping;
        l.mapping.soft = (own & OwnSoft) ? n.soft : soft;
        l.mapping.revision = rev + 1;
    }
    if (own & OwnViewportOpacity) l.viewportOpacity = n.viewportOpacity;
    if (own & OwnSpeed) {
        if (!l.hasTransport() && l.generator) {
            l.generator->speed = std::clamp(n.speed, 0.0, 10.0);
        } else {
            const int dir = n.speed < 0 ? -1 : n.speed > 0 ? 1 : l.dir;
            const double pos = l.position();
            l.speed = n.speed;
            if (dir != l.dir && l.hasTransport()) reposition(l, pos, dir); // the other way from the same place
        }
    }
    if (own & OwnInOut) {
        l.inPoint = n.inPoint;
        l.outPoint = n.outPoint;
    }
    auto has = [own](int k) { return (own & (OwnText0 << k)) != 0; };
    if (has(TextSize)) l.text.size = int(std::lround(n.text.size));
    if (has(TextLineHeight)) l.text.lineHeight = n.text.lineHeight;
    if (has(TextLetterSpacing)) l.text.letterSpacing = n.text.letterSpacing;
    if (has(TextOutline)) l.text.outline = n.text.outline;
    if (has(TextShadowX)) l.text.shadowX = n.text.shadowX;
    if (has(TextShadowY)) l.text.shadowY = n.text.shadowY;
    if (has(TextColor)) l.text.color = n.text.color;
    if (has(TextOutlineColor)) l.text.outlineColor = n.text.outlineColor;
    if (has(TextShadowColor)) l.text.shadowColor = n.text.shadowColor;
    if (!(own & OwnIsf)) return;
    for (size_t k = 0; k < l.effects.size() && k < n.fxSpeed.size(); ++k) l.effects[k]->speed = std::clamp(n.fxSpeed[k], 0.0, 10.0);
    auto apply = [](IsfInstance *inst, const std::vector<IsfValue> &v) {
        if (!inst) return;
        for (size_t k = 0; k < v.size() && k < inst->inputs().size(); ++k) inst->inputs()[k].setValue(v[k]);
    };
    if (!n.isf.empty()) apply(l.generator.get(), n.isf[0]);
    for (size_t k = 0; k < l.effects.size() && k + 1 < n.isf.size(); ++k) apply(l.effects[k].get(), n.isf[k + 1]);
}

static double mixd(double a, double b, double t) { return a + (b - a) * t; }
static float mixf(float a, float b, double t) { return float(a + (b - a) * t); }
static QPointF mixp(QPointF a, QPointF b, double t) { return a + (b - a) * t; }
static QColor mixc(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(mixf(a.redF(), b.redF(), t), mixf(a.greenF(), b.greenF(), t), mixf(a.blueF(), b.blueF(), t),
                            mixf(a.alphaF(), b.alphaF(), t));
}

// Progress of a number `elapsed` seconds into its own time: 1 for a cut, eased with given curve otherwise
static double progress(double elapsed, double duration, EasingCurve curve = EasingCurve::EaseInOut)
{
    if (duration <= 0) return 1.0;
    const double t = std::min(1.0, elapsed / duration);
    return applyEasing(t, curve);
}

static LayerNumbers mixNumbers(const LayerNumbers &a, const LayerNumbers &b, const LayerTimes &d, double elapsed)
{
    auto t = [elapsed](double duration, EasingCurve curve = EasingCurve::EaseInOut) {
        return progress(elapsed, duration, curve);
    };
    LayerNumbers n = b;
    n.opacity = mixf(a.opacity, b.opacity, t(d.opacity, d.opacityCurve));
    n.volume = mixf(a.volume, b.volume, t(d.volume, d.volumeCurve));
    const double tr = t(d.roi, d.roiCurve);
    n.roi = QRectF(mixp(a.roi.topLeft(), b.roi.topLeft(), tr), mixp(a.roi.bottomRight(), b.roi.bottomRight(), tr));
    const double tc = t(d.temp, d.colorCurve);
    n.color.temp = mixf(a.color.temp, b.color.temp, tc);
    n.color.tint = mixf(a.color.tint, b.color.tint, tc);
    for (int c = 0; c < 3; ++c) {
        n.color.add[c] = mixf(a.color.add[c], b.color.add[c], tc);
        n.color.remove[c] = mixf(a.color.remove[c], b.color.remove[c], tc);
    }
    if (a.mapping.cols == b.mapping.cols && a.mapping.rows == b.mapping.rows) {
        const double tm = t(d.mapping, d.mappingCurve);
        for (int k = 0; k < 4; ++k) n.mapping.corners[k] = mixp(a.mapping.corners[k], b.mapping.corners[k], tm);
        for (size_t k = 0; k < n.mapping.offsets.size() && k < a.mapping.offsets.size(); ++k)
            n.mapping.offsets[k] = mixp(a.mapping.offsets[k], b.mapping.offsets[k], tm);
    }
    n.mapping.pivot = mixp(a.mapping.pivot, b.mapping.pivot, t(d.pivot, d.pivotCurve));
    // Soft edge: interpolate width and power per side
    {
        const double ts = t(d.softEdge, d.softEdgeCurve);
        for (int side = 0; side < 4; ++side) {
            n.soft.width[side] = mixf(a.soft.width[side], b.soft.width[side], ts);
            n.soft.power[side] = mixf(a.soft.power[side], b.soft.power[side], ts);
        }
    }
    // Viewport opacity: interpolate all viewports from both source and target
    {
        const double tv = t(d.viewportOpacity, d.viewportOpacityCurve);
        n.viewportOpacity.clear();
        QSet<quint64> allViewports;
        for (const auto &[vp, op] : a.viewportOpacity) allViewports.insert(vp);
        for (const auto &[vp, op] : b.viewportOpacity) allViewports.insert(vp);
        for (quint64 vp : allViewports) {
            const float opA = a.viewportOpacity.count(vp) ? a.viewportOpacity.at(vp) : 1.0f;
            const float opB = b.viewportOpacity.count(vp) ? b.viewportOpacity.at(vp) : 1.0f;
            n.viewportOpacity[vp] = mixf(opA, opB, tv);
        }
    }
    // Playback speed and play range
    {
        n.speed = mixd(a.speed, b.speed, t(d.speed, d.speedCurve));
        n.fxSpeed = b.fxSpeed;
        for (size_t k = 0; k < n.fxSpeed.size() && k < a.fxSpeed.size() && k < d.fxSpeed.size(); ++k)
            n.fxSpeed[k] = mixd(a.fxSpeed[k], b.fxSpeed[k], t(d.fxSpeed[k], d.fxSpeedCurves[k]));
        n.inPoint = mixd(a.inPoint, b.inPoint, t(d.inPoint, d.inOutCurve));
        n.outPoint = mixd(a.outPoint, b.outPoint, t(d.outPoint, d.inOutCurve));
    }
    // Text generator
    {
        auto tt = [&](int k) { return t(d.text[k], d.textCurve[k]); };
        const TextNumbers &x = a.text, &y = b.text;
        n.text.size = mixf(x.size, y.size, tt(TextSize));
        n.text.lineHeight = mixf(x.lineHeight, y.lineHeight, tt(TextLineHeight));
        n.text.letterSpacing = mixf(x.letterSpacing, y.letterSpacing, tt(TextLetterSpacing));
        n.text.outline = mixf(x.outline, y.outline, tt(TextOutline));
        n.text.shadowX = mixf(x.shadowX, y.shadowX, tt(TextShadowX));
        n.text.shadowY = mixf(x.shadowY, y.shadowY, tt(TextShadowY));
        n.text.color = mixc(x.color, y.color, tt(TextColor));
        n.text.outlineColor = mixc(x.outlineColor, y.outlineColor, tt(TextOutlineColor));
        n.text.shadowColor = mixc(x.shadowColor, y.shadowColor, tt(TextShadowColor));
    }
    for (size_t i = 0; i < n.isf.size() && i < a.isf.size(); ++i)
        for (size_t k = 0; k < n.isf[i].size() && k < a.isf[i].size(); ++k) {
            const double tk = i < d.isf.size() && k < d.isf[i].size() ? t(d.isf[i][k]) : 1.0;
            IsfValue &v = n.isf[i][k];
            const IsfValue &f = a.isf[i][k];
            v.f = mixd(f.f, v.f, tk);
            v.p = mixp(f.p, v.p, tk);
            for (int c = 0; c < 4; ++c) v.c[c] = mixf(f.c[c], v.c[c], tk);
            // bools and lists: the target, at once
        }
    return n;
}

// Times of a layer's numbers: the snapshot's fade, or the time the snapshot gives that value (key → seconds)
static LayerTimes timesOf(const Layer &l, const QJsonObject &timing, double fade)
{
    auto time = [&](const QString &key) {
        const QJsonValue v = timing.value(key);
        return v.isDouble() ? std::clamp(v.toDouble(), 0.0, 600.0) : fade;
    };
    auto curve = [&](const QString &key) {
        const QJsonValue v = timing.value(key + "/curve");
        return easingCurveFromKey(v.toString());
    };
    LayerTimes d;
    d.opacity = time(QStringLiteral("opacity"));
    d.volume = time(QStringLiteral("source/volume"));
    d.roi = time(QStringLiteral("roi"));
    d.temp = time(QStringLiteral("color/temp"));
    d.tint = time(QStringLiteral("color/tint"));
    d.add = time(QStringLiteral("color/add"));
    d.remove = time(QStringLiteral("color/remove"));
    d.mapping = time(QStringLiteral("spatial"));
    d.pivot = timing.value(QStringLiteral("spatial/pivot")).isDouble() ? time(QStringLiteral("spatial/pivot")) : 0.0; // CUT by default
    d.viewportOpacity = time(QStringLiteral("viewport"));
    d.softEdge = time(QStringLiteral("spatial/soft_edge"));
    d.speed = time(QStringLiteral("source/speed"));
    d.inPoint = time(QStringLiteral("source/in"));
    d.outPoint = time(QStringLiteral("source/out"));
    for (int k = 0; k < TextNumCount; ++k) {
        const QString key = QString::fromLatin1(kTextTimeKeys[k]);
        d.text[k] = time(key);
        // The typing goes at an even pace unless the snapshot gives it a curve
        d.textCurve[k] = k == TextContent && !timing.contains(key + "/curve") ? EasingCurve::Linear : curve(key);
    }
    // Read easing curves for each parameter
    d.opacityCurve = curve(QStringLiteral("opacity"));
    d.volumeCurve = curve(QStringLiteral("source/volume"));
    d.roiCurve = curve(QStringLiteral("roi"));
    d.colorCurve = curve(QStringLiteral("color/temp")); // use temp for all color parameters
    d.mappingCurve = curve(QStringLiteral("spatial"));
    d.pivotCurve = curve(QStringLiteral("spatial/pivot"));
    d.softEdgeCurve = curve(QStringLiteral("spatial/soft_edge"));
    d.viewportOpacityCurve = curve(QStringLiteral("viewport"));
    d.speedCurve = curve(QStringLiteral("source/speed"));
    d.inOutCurve = curve(QStringLiteral("source/in")); // the curve of in is also the one of out
    auto params = [&](const IsfInstance *inst, const QString &base) {
        std::vector<double> v;
        if (inst)
            for (const IsfInput &in : inst->inputs()) v.push_back(time(base + osc::safeName(in.name))); // its address
        return v;
    };
    d.isf.push_back(params(l.generator.get(), QStringLiteral("source/param/")));
    QStringList fxNames;
    for (const auto &x : l.effects) fxNames << x->name();
    const QStringList fxSegs = osc::uniqueSegments(fxNames);
    for (size_t k = 0; k < l.effects.size(); ++k) {
        d.isf.push_back(params(l.effects[k].get(), QStringLiteral("fx/%1/param/").arg(fxSegs[int(k)])));
        const QString key = QStringLiteral("fx/%1/speed").arg(fxSegs[int(k)]);
        d.fxSpeed.push_back(time(key));
        d.fxSpeedCurves.push_back(curve(key));
    }
    return d;
}

// The key of the time (and easing) of a value of a layer, from its path in the layer's JSON: the OSC address of that
// value, or of the group of values that fade together (the ROI, the spatial shape, a color's three components…)
QString Engine::timingKey(const QStringList &path, const QJsonObject &layer)
{
    if (path.isEmpty()) return {};
    const QString &a = path[0];
    if (a == "opacity") return a;
    if (a == "viewports") return QStringLiteral("viewport");
    if (a == "spatial" && path.size() >= 2 && (path[1] == "soft_edge" || path[1] == "pivot")) return QStringLiteral("spatial/") + path[1];
    if (a == "spatial") return QStringLiteral("spatial");
    if (a == "roi") return QStringLiteral("roi");
    if (a == "volume") return QStringLiteral("source/volume");
    if (a == "source" && path.size() >= 2) {
        for (int k = 0; k < TextNumCount; ++k)
            if (path[1] == QLatin1String(kTextNumKeys[k])) return QString::fromLatin1(kTextTimeKeys[k]); // Text generator
        if (path[1] == "speed" || path[1] == "in" || path[1] == "out") return QStringLiteral("source/") + path[1];
        if (path[1] == "params" && path.size() >= 3) return QStringLiteral("source/param/") + osc::safeName(path[2]);
    }
    if (a == "color" && path.size() >= 2 && (path[1] == "temp" || path[1] == "tint" || path[1] == "add" || path[1] == "remove"))
        return QStringLiteral("color/") + path[1];
    if (a == "fx" && path.size() == 3 && path[2] == "speed") {
        QStringList names;
        for (const QJsonValue &v : layer.value("fx").toArray())
            names << QFileInfo(v.toObject().value("path").toString()).completeBaseName();
        const int k = path[1].toInt();
        if (k < 0 || k >= names.size()) return {};
        return QStringLiteral("fx/%1/speed").arg(osc::uniqueSegments(names).at(k));
    }
    if (a == "fx" && path.size() >= 4 && path[2] == "params") {
        QStringList names;
        for (const QJsonValue &v : layer.value("fx").toArray())
            names << QFileInfo(v.toObject().value("path").toString()).completeBaseName();
        const int k = path[1].toInt();
        if (k < 0 || k >= names.size()) return {};
        return QStringLiteral("fx/%1/param/%2").arg(osc::uniqueSegments(names).at(k), osc::safeName(path[3]));
    }
    return {};
}

// ISF parameter values of a saved instance, onto the current values (by input name)
static void readParams(const IsfInstance *inst, const QJsonObject &params, std::vector<IsfValue> &values)
{
    if (!inst) return;
    for (size_t k = 0; k < inst->inputs().size() && k < values.size(); ++k) {
        const IsfInput &in = inst->inputs()[k];
        if (!params.contains(in.name)) continue;
        const QJsonValue v = params.value(in.name);
        IsfValue &x = values[k];
        switch (in.type) {
        case IsfInput::Float: x.f = v.toDouble(x.f); break;
        case IsfInput::Bool: x.b = v.toBool(x.b); break;
        case IsfInput::Long: x.l = v.toInt(x.l); break;
        case IsfInput::Point2D: {
            const QJsonArray a = v.toArray();
            if (a.size() >= 2) x.p = QPointF(a[0].toDouble(), a[1].toDouble());
            break;
        }
        case IsfInput::Color: {
            const QJsonArray a = v.toArray();
            for (int c = 0; c < 4 && c < a.size(); ++c) x.c[c] = float(a[c].toDouble());
            break;
        }
        default: break;
        }
    }
}

static QStringList effectPaths(const QJsonArray &a)
{
    QStringList p;
    for (const QJsonValue &v : a) p << QDir::cleanPath(v.toObject().value("path").toString());
    return p;
}

// ---------------------------------------------------------------------------

int Engine::snapshotCount() const
{
    Lock lk(&m_mutex);
    return int(m_snapshots.size());
}

Engine::Snapshot Engine::snapshot(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_snapshots.size()) ? m_snapshots[size_t(i)] : Snapshot();
}

int Engine::indexOfSnapshot(quint64 id) const
{
    Lock lk(&m_mutex);
    for (size_t i = 0; i < m_snapshots.size(); ++i)
        if (m_snapshots[i].id == id) return int(i);
    return -1;
}

void Engine::setSnapshot(int i, const Snapshot &m)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_snapshots.size())) return;
        const quint64 id = m_snapshots[size_t(i)].id; // it keeps its identity
        m_snapshots[size_t(i)] = m;
        m_snapshots[size_t(i)].id = id;
    }
    emit snapshotsChanged();
}

int Engine::addSnapshot(const Snapshot &m, int at)
{
    {
        Lock lk(&m_mutex);
        if (at < 0 || at > int(m_snapshots.size())) at = int(m_snapshots.size());
        Snapshot copy = m;
        bool taken = !copy.id;
        for (const Snapshot &o : m_snapshots) taken = taken || o.id == copy.id;
        if (taken) copy.id = m_nextSnapshotId++;
        m_nextSnapshotId = std::max(m_nextSnapshotId, copy.id + 1);
        m_snapshots.insert(m_snapshots.begin() + at, copy);
    }
    emit snapshotsChanged();
    return at;
}

void Engine::removeSnapshot(int i)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_snapshots.size())) return;
        m_snapshots.erase(m_snapshots.begin() + i);
    }
    emit snapshotsChanged();
}

QJsonArray Engine::captureLayers() const
{
    Lock lk(&m_mutex);
    QJsonArray a;
    for (int i = 0; i < int(m_layers.size()); ++i) {
        if (m_layers[size_t(i)]->isViewport) continue; // a viewport has one state, not one per snapshot
        QJsonObject o = layerJson(i);
        o.remove("anims"); // the layer's animations are not part of a snapshot: a recall leaves them alone
        o["included"] = true;
        a.append(o);
    }
    return a;
}

Engine::RecallProgress Engine::recallProgress() const
{
    Lock lk(&m_mutex);
    RecallProgress r;
    r.snapshot = m_recalledSnapshot;
    r.total = m_recallTotal;
    r.elapsed = m_fades.empty() && m_transitions.empty() && !m_compFade.opacity && !m_compFade.volume ? m_recallTotal
                                                                                                  : m_fadeElapsed;
    return r;
}

bool Engine::isFading() const
{
    Lock lk(&m_mutex);
    return !m_fades.empty();
}

QJsonObject Engine::captureComposition() const
{
    return QJsonObject{{"included", true},
                       {"opacity", compositionOpacityTarget()},
                       {"volume", double(audioVolume())}};
}

void Engine::applyComposition(const QJsonObject &c, double fade)
{
    if (c.isEmpty() || !c.value("included").toBool(true)) return;
    const QJsonObject timing = c.value("timing").toObject();
    auto time = [&](const QString &key) {
        const QJsonValue v = timing.value(key);
        return v.isDouble() ? std::clamp(v.toDouble(), 0.0, 600.0) : std::max(0.0, fade);
    };
    auto curve = [&](const QString &key) { return int(easingCurveFromKey(timing.value(key + "/curve").toString())); };
    const bool hasOpacity = c.contains("opacity"), hasVolume = c.contains("volume");
    const double opacity = std::clamp(c.value("opacity").toDouble(1), 0.0, 1.0);
    const float volume = float(std::clamp(c.value("volume").toDouble(1), 0.0, 2.0));
    const double opacityDur = time(QStringLiteral("opacity")), volumeDur = time(QStringLiteral("volume"));
    if (hasOpacity && opacityDur <= 0) fadeCompositionOpacity(opacity, 0);
    if (hasVolume && volumeDur <= 0) setAudioVolume(volume);
    // A value the snapshot holds stops the fade another snapshot gives it; the other one goes on
    Lock lk(&m_mutex);
    CompositionFade &f = m_compFade;
    if (hasOpacity) {
        f.opacity = opacityDur > 0;
        f.opacityElapsed = 0;
        f.opacityFrom = m_compositionOpacity.load();
        f.opacityTo = opacity;
        f.opacityDur = opacityDur;
        f.opacityCurve = curve(QStringLiteral("opacity"));
    }
    if (hasVolume) {
        f.volume = volumeDur > 0;
        f.volumeElapsed = 0;
        f.volumeFrom = m_audio->volume();
        f.volumeTo = volume;
        f.volumeDur = volumeDur;
        f.volumeCurve = curve(QStringLiteral("volume"));
    }
}

void Engine::stepCompositionFade(double dt)
{
    CompositionFade &f = m_compFade;
    dt = std::max(0.0, dt);
    if (f.opacity) {
        f.opacityElapsed += dt;
        const double p = progress(f.opacityElapsed, f.opacityDur, EasingCurve(f.opacityCurve));
        m_compositionOpacityTarget = mixd(f.opacityFrom, f.opacityTo, p);
        m_compositionOpacitySpeed = 0; // the frame takes it as it is
        if (f.opacityElapsed >= f.opacityDur) f.opacity = false;
    }
    if (f.volume) {
        f.volumeElapsed += dt;
        const double p = progress(f.volumeElapsed, f.volumeDur, EasingCurve(f.volumeCurve));
        m_audio->setVolume(mixf(f.volumeFrom, f.volumeTo, p));
        if (f.volumeElapsed >= f.volumeDur) f.volume = false;
    }
}

void Engine::recallSnapshot(int i)
{
    const Snapshot m = snapshot(i);
    if (m.layers.isEmpty() && m.composition.isEmpty()) return;
    if (!m.layers.isEmpty()) applyLayers(m.layers, m.fade, true);
    applyComposition(m.composition, m.fade);
    {
        Lock lk(&m_mutex);
        m_recalledSnapshot = m.id;
        double total = 0;
        for (const auto &job : m_fades)
            if (job->elapsed <= 0) total = std::max(total, job->times.longest()); // this recall's, not the older ones
        for (const auto &[id, t] : m_transitions) total = std::max(total, t->duration - t->elapsed);
        if (m_compFade.opacity && m_compFade.opacityElapsed <= 0) total = std::max(total, m_compFade.opacityDur);
        if (m_compFade.volume && m_compFade.volumeElapsed <= 0) total = std::max(total, m_compFade.volumeDur);
        m_recallTotal = total;
    }
    emit snapshotRecalled(i);
}

void Engine::applyLayers(const QJsonArray &layers, double fade, bool hideOthers)
{
    QSet<quint64> named; // the layers the state speaks of (left out or not)
    for (const QJsonValue &v : layers) named.insert(v.toObject().value("id").toString().toULongLong());
    // The snapshots still running go on: this recall stops their fades only for the values it sets itself
    // (from where they are now), and leaves them the others
    auto takeOver = [this](quint64 id, quint64 values) {
        Lock lk(&m_mutex);
        for (auto it = m_fades.begin(); it != m_fades.end();) {
            FadeJob &j = **it;
            if (j.id == id) {
                j.owned &= ~values;
                if (values & OwnOpacity) j.hideAtEnd = false;
            }
            it = j.owned ? it + 1 : m_fades.erase(it);
        }
    };
    std::vector<std::shared_ptr<FadeJob>> jobs;
    for (const QJsonValue &value : layers) {
        QJsonObject o = value.toObject();
        o.remove("anims"); // not part of a snapshot
        if (!o.value("included").toBool(true) || o.value("viewport").toBool()) continue;
        const quint64 id = o.value("id").toString().toULongLong();
        int idx = indexOfId(id);
        if (idx >= 0 && isLocked(idx)) continue; // a locked layer is not changed by a snapshot
        if (idx < 0) { // removed since: recreated at the bottom
            takeOver(id, kOwnAll);
            insertLayerJson(layerCount(), o);
            continue;
        }
        const QJsonObject cur = layerJson(idx);
        const QJsonObject curSrc = cur.value("source").toObject(), src = o.value("source").toObject();
        const bool group = o.value("group").toBool();
        if (!group && (curSrc.value("type") != src.value("type") || curSrc.value("layer") != src.value("layer") ||
                       curSrc.value("tap") != src.value("tap") ||
                       QDir::cleanPath(curSrc.value("path").toString()) != QDir::cleanPath(src.value("path").toString()))) {
            // Another media: it comes in with the layer's transition over the source's time (the snapshot's
            // fade unless it has its own), or at once for a cut
            const QJsonValue own = o.value("timing").toObject().value("source/file");
            const double t = own.isDouble() ? std::clamp(own.toDouble(), 0.0, 600.0) : std::max(0.0, fade);
            takeOver(id, kOwnAll);
            // The layer is made again with its new source: its animations go on as they were (not restarted)
            std::vector<Animation> anims;
            {
                Lock lk(&m_mutex);
                anims = layer(idx)->anims;
            }
            auto keepAnims = [this, id, &anims] {
                Lock lk(&m_mutex);
                if (Layer *l = layer(indexOfId(id))) l->anims = anims;
            };
            if (t <= 0) {
                replaceLayerJson(idx, o);
                keepAnims();
                continue;
            }
            startSourceTransition(idx, o, t);
            keepAnims();
            // The layer now holds the snapshot's state; its numbers move there from the outgoing one's (ROI,
            // color, mapping, opacity, volume) over their times, as they would with the same source
            Lock lk(&m_mutex);
            Layer *l = layer(indexOfId(id));
            const auto tr = m_transitions.find(id);
            if (!l || tr == m_transitions.end()) continue;
            const Layer &old = *tr->second->from;
            auto job = std::make_shared<FadeJob>();
            job->id = id;
            job->to = numbersOf(*l);
            job->from = job->to;
            job->from.opacity = old.enabled ? old.opacity : 0.0f;
            job->from.volume = old.volume;
            job->from.roi = old.roi;
            job->from.color = old.color;
            job->from.viewportOpacity = old.viewportOpacity;
            job->from.soft = old.mapping.soft;
            if (old.mapping.cols == l->mapping.cols && old.mapping.rows == l->mapping.rows) job->from.mapping = old.mapping;
            job->times = timesOf(*l, o.value("timing").toObject(), std::max(0.0, fade));
            setNumbers(*l, mixNumbers(job->from, job->to, job->times, 0));
            jobs.push_back(job);
            continue;
        }
        if (effectPaths(cur.value("fx").toArray()) != effectPaths(o.value("fx").toArray()))
            setEffectsJson(idx, o.value("fx").toArray()); // another chain: at once
        if (src.contains("play_mode")) {
            setLayerPlayMode(idx, playModeFromKey(src.value("play_mode").toString()));
            setLayerInOut(idx, src.value("in").toDouble(0), src.value("out").toDouble(-1));
        }

        Lock lk(&m_mutex);
        Layer *l = layer(idx);
        if (!l) continue;
        l->name = o.value("name").toString(l->name);
        l->blend = blendModeFromKey(o.value("blend_mode").toString(blendModeKey(l->blend)));
        l->effectsEnabled = o.value("fx_enable").toBool(l->effectsEnabled);
        l->muted = o.value("muted").toBool(l->muted);
        // Which viewports it is drawn in: a snapshot can send a layer to another projector. Read into the
        // fade's target (not onto the layer) so it moves there from the current values
        std::map<quint64, float> targetViewportOpacity;
        const QJsonObject vo = o.value("viewports").toObject();
        for (auto it = vo.begin(); it != vo.end(); ++it)
            targetViewportOpacity[it.key().toULongLong()] = float(std::clamp(it.value().toDouble(1.0), 0.0, 1.0));
        const QJsonArray fx = o.value("fx").toArray();
        std::vector<double> fxSpeedBefore;
        for (const auto &e : l->effects) fxSpeedBefore.push_back(e->speed);
        for (size_t k = 0; k < l->effects.size() && int(k) < fx.size(); ++k) {
            l->effects[k]->readState(fx[int(k)].toObject()); // on, mask
            l->effects[k]->speed = fxSpeedBefore[k];         // its pace fades with the other numbers
        }
        // The color section's switches and mask: at once (only its numbers fade)
        const QJsonObject colorState = o.value("color").toObject();
        if (!colorState.isEmpty()) {
            ColorAdjust c;
            colorFromJson(c, colorState);
            l->color.enabled = c.enabled;
            l->color.tempOn = c.tempOn;
            l->color.tintOn = c.tintOn;
            l->color.addOn = c.addOn;
            l->color.removeOn = c.removeOn;
            l->color.maskLayer = c.maskLayer;
            l->color.maskInvert = c.maskInvert;
        }

        auto job = std::make_shared<FadeJob>();
        job->id = id;
        job->from = numbersOf(*l);
        job->times = timesOf(*l, o.value("timing").toObject(), std::max(0.0, fade));
        LayerNumbers &to = job->to;
        to = job->from;
        to.opacity = float(o.value("opacity").toDouble(to.opacity));
        to.volume = float(std::clamp(o.value("volume").toDouble(to.volume), 0.0, 2.0));
        const QJsonArray roi = o.value("roi").toArray();
        if (roi.size() == 4)
            to.roi = QRectF(QPointF(roi[0].toDouble(), roi[1].toDouble()), QPointF(roi[2].toDouble(), roi[3].toDouble()));
        const QJsonObject color = o.value("color").toObject();
        if (!color.isEmpty()) {
            to.color.temp = float(color.value("temp").toDouble(0));
            to.color.tint = float(color.value("tint").toDouble(0));
            for (int c = 0; c < 3; ++c) {
                to.color.add[c] = float(color.value("add").toArray().at(c).toDouble(0));
                to.color.remove[c] = float(color.value("remove").toArray().at(c).toDouble(0));
            }
        }
        if (o.contains("spatial")) {
            to.mapping.fromJson(o.value("spatial").toObject());
            to.soft = to.mapping.soft; // setNumbers applies `soft` over the mapping's: the snapshot's crop
        }
        to.viewportOpacity = targetViewportOpacity;
        job->from.fxSpeed = fxSpeedBefore;
        if (l->hasTransport()) {
            if (src.contains("play_mode")) to.speed = src.value("speed").toDouble(1.0);
        } else if (l->generator) {
            to.speed = std::clamp(src.value("speed").toDouble(1.0), 0.0, 10.0);
        }
        for (size_t k = 0; k < to.fxSpeed.size() && int(k) < fx.size(); ++k)
            to.fxSpeed[k] = std::clamp(fx[int(k)].toObject().value("speed").toDouble(1.0), 0.0, 10.0);
        if (l->type == SourceType::Text && src.value("type").toString() == "text") {
            // Text generator: its numbers move to the snapshot's, its switches and words are set at once, its
            // text is typed over its time
            TextSource target = l->text;
            readTextJson(target, src);
            to.text = textNumbersOf(target);
            TextSource &t = l->text;
            t.font = target.font;
            t.align = target.align;
            t.bold = target.bold;
            t.italic = target.italic;
            t.underline = target.underline;
            t.strike = target.strike;
            t.shadow = target.shadow;
            t.width = target.width;
            t.height = target.height;
            if (target.content != t.content) {
                const double dur = job->times.text[TextContent];
                const QString from = t.shown(); // what is on screen now (a recall may interrupt another typing)
                t.content = target.content;
                t.stopTyping();
                if (dur > 0) {
                    t.typedFrom = from;
                    t.typeDur = dur;
                    t.typeProgress = 0;
                    t.typeCurve = int(job->times.textCurve[TextContent]);
                }
            }
        }
        // The values this state sets: their fades from other snapshots stop here
        job->owned = OwnOpacity | OwnVolume | OwnViewportOpacity | OwnIsf;
        if (roi.size() == 4) job->owned |= OwnRoi;
        if (!color.isEmpty()) job->owned |= OwnColor;
        if (o.contains("spatial")) job->owned |= OwnMapping | OwnSoft;
        if (src.contains("play_mode")) job->owned |= OwnSpeed | OwnInOut;
        if (!l->hasTransport() && l->generator) job->owned |= OwnSpeed;
        if (l->type == SourceType::Text && src.value("type").toString() == "text") job->owned |= kOwnText;
        takeOver(id, job->owned);
        if (!to.isf.empty()) readParams(l->generator.get(), src.value("params").toObject(), to.isf[0]);
        for (size_t k = 0; k < l->effects.size() && k + 1 < to.isf.size() && int(k) < fx.size(); ++k)
            readParams(l->effects[k].get(), fx[int(k)].toObject().value("params").toObject(), to.isf[k + 1]);

        // Visibility: shown at once and faded in from 0, or faded out then hidden
        const bool visible = o.value("enable").toBool(true);
        if (visible && !l->enabled) {
            l->enabled = true;
            job->from.opacity = 0;
        } else if (!visible && l->enabled) {
            job->hideAtEnd = true;
            job->finalOpacity = to.opacity;
            to.opacity = 0;
        }
        if (job->times.longest() <= 0) {
            setNumbers(*l, to, job->owned);
            if (job->hideAtEnd) {
                l->enabled = false;
                l->opacity = job->finalOpacity;
            }
        } else {
            // Bools and lists reach their target at once; the numbers start from where they are (a cut: there)
            setNumbers(*l, mixNumbers(job->from, to, job->times, 0), job->owned);
            if (job->hideAtEnd && job->times.opacity <= 0) { // hidden by a cut
                l->enabled = false;
                l->opacity = job->finalOpacity;
                job->hideAtEnd = false;
                job->from.opacity = job->to.opacity = job->finalOpacity;
            }
            jobs.push_back(job);
        }
    }
    fixLayerReferences(); // layers re-created or sources changed: no reference left dangling or looping
    Lock lk(&m_mutex);
    // The layers the snapshot does not know (created since): faded out with the snapshot's fade, then hidden
    if (hideOthers)
        for (auto &lp : m_layers) {
            Layer &l = *lp;
            if (l.isViewport || named.contains(l.id) || !l.enabled || isLocked(indexOfId(l.id))) continue;
            takeOver(l.id, OwnOpacity); // only its opacity: what other snapshots fade on it goes on
            if (fade <= 0) {
                l.enabled = false;
                continue;
            }
            auto job = std::make_shared<FadeJob>();
            job->id = l.id;
            job->owned = OwnOpacity;
            job->from = numbersOf(l);
            job->to = job->from;
            job->to.opacity = 0;
            job->finalOpacity = l.opacity;
            job->hideAtEnd = true;
            job->times = timesOf(l, QJsonObject(), fade);
            jobs.push_back(job);
        }
    for (auto &j : jobs) m_fades.push_back(std::move(j));
    m_fadeElapsed = 0;
}

// The part of a fade that is this number alone (0: it shares its part with other numbers)
static quint64 ownBitOf(const QString &path)
{
    if (path == QLatin1String("opacity")) return OwnOpacity;
    if (path == QLatin1String("source/volume")) return OwnVolume;
    if (path == QLatin1String("source/speed")) return OwnSpeed; // not set then set back: no turning round twice a frame
    for (int k = 0; k < TextNumCount; ++k)
        if (path == QLatin1String(kTextTimeKeys[k])) return OwnText0 << k;
    return 0;
}

// Every number moves on its own time, each snapshot on its own clock; a layer that fades out is hidden once its
// opacity got there
void Engine::stepFade(double dt)
{
    dt = std::max(0.0, dt);
    m_fadeElapsed += dt;
    for (auto it = m_fades.begin(); it != m_fades.end();) {
        FadeJob &job = **it;
        job.elapsed += dt;
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == job.id) l = x.get();
        if (!l) {
            it = m_fades.erase(it);
            continue;
        }
        // What an animation drives stays as it set it: a value the fade drives alone is left out of it, one that shares
        // its part with others (a side of the ROI, a corner of the mapping…) is put back after it
        quint64 owned = job.owned;
        std::vector<std::pair<QString, double>> keep;
        for (const QString &path : job.released) {
            if (const quint64 bit = ownBitOf(path)) {
                owned &= ~bit;
                continue;
            }
            double v = 0;
            if (layerNumber(*l, path, &v, nullptr, m_compSize)) keep.push_back({path, v});
        }
        setNumbers(*l, mixNumbers(job.from, job.to, job.times, job.elapsed), owned);
        for (const auto &[path, v] : keep) layerNumber(*l, path, nullptr, &v, m_compSize);
        if (job.hideAtEnd && job.elapsed >= job.times.opacity) {
            l->enabled = false;
            if (!job.released.contains(QStringLiteral("opacity"))) l->opacity = job.finalOpacity;
            job.hideAtEnd = false;
            job.from.opacity = job.to.opacity = job.finalOpacity; // stays as stored while the rest moves on
        }
        it = job.elapsed < job.times.longest() ? it + 1 : m_fades.erase(it);
    }
}

void Engine::releaseFromFades(quint64 layer, const QString &path)
{
    for (const auto &job : m_fades)
        if (job->id == layer && !job->released.contains(path)) job->released << path;
}

void Engine::advanceFades(double dt)
{
    Lock lk(&m_mutex);
    stepFade(dt);
    stepTransitions(dt);
    stepTypewriters(dt);
    stepCompositionFade(dt);
    stepAnimations(dt);
}

void Engine::stepTypewriters(double dt)
{
    for (auto &lp : m_layers) {
        TextSource &t = lp->text;
        if (t.typeDur <= 0) continue;
        t.typeElapsed += std::max(0.0, dt);
        if (t.typeElapsed >= t.typeDur) t.stopTyping();
        else t.typeProgress = applyEasing(t.typeElapsed / t.typeDur, EasingCurve(t.typeCurve));
    }
}

// ---------------------------------------------------------------------------
// Source transitions

void Engine::setDefaultTransition(const QString &path)
{
    Lock lk(&m_mutex);
    m_defaultTransition = path;
}

QString Engine::defaultTransition() const
{
    Lock lk(&m_mutex);
    return m_defaultTransition;
}

bool Engine::isTransitioning(quint64 layer) const
{
    Lock lk(&m_mutex);
    return m_transitions.count(layer) > 0;
}

// The layer keeps its place and its id; the object that held the outgoing source becomes the transition's
// `from` and goes on playing (picture and sound) until the transition is over.
void Engine::startSourceTransition(int index, const QJsonObject &state, double duration)
{
    std::unique_ptr<Layer> old;
    {
        Lock lk(&m_mutex);
        if (index < 0 || index >= int(m_layers.size()) || m_layers[size_t(index)]->isGroup) return;
        const quint64 id = m_layers[size_t(index)]->id;
        // A transition already running there: its outgoing source goes, what is shown now becomes the outgoing one
        auto it = m_transitions.find(id);
        if (it != m_transitions.end()) {
            retireTransition(std::move(it->second));
            m_transitions.erase(it);
        }
        old = std::move(m_layers[size_t(index)]);
        m_layers.erase(m_layers.begin() + index);
    }
    const quint64 id = old->id;
    insertLayerJson(index, state); // same id: free again
    Lock lk(&m_mutex);
    Layer *now = layer(indexOfId(id));
    if (!now || now->id != id) { // could not take its place: no transition
        auto t = std::make_unique<SourceTransition>();
        t->from = std::move(old);
        retireTransition(std::move(t));
        return;
    }
    auto t = std::make_unique<SourceTransition>();
    t->shaderPath = now->transition.isEmpty() ? m_defaultTransition : now->transition;
    t->duration = std::max(1e-3, duration);
    old->transitionGain = 1.0f;
    now->transitionGain = 0.0f;
    t->from = std::move(old);
    m_transitions[id] = std::move(t);
}

// The outgoing sources go on (clock, sound); a transition whose layer is gone, or that is over, ends
void Engine::stepTransitions(double dt)
{
    for (auto it = m_transitions.begin(); it != m_transitions.end();) {
        SourceTransition &t = *it->second;
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == it->first) l = x.get();
        t.elapsed += std::max(0.0, dt);
        if (!l || t.elapsed >= t.duration) {
            if (l) l->transitionGain = 1.0f;
            retireTransition(std::move(it->second));
            it = m_transitions.erase(it);
            continue;
        }
        const float p = float(progress(t.elapsed, t.duration));
        l->transitionGain = p;
        t.from->transitionGain = 1.0f - p;
        t.from->enabled = l->enabled; // heard as the layer is
        t.from->parentEnabled = l->parentEnabled;
        ++it;
    }
}

void Engine::retireTransition(std::unique_ptr<SourceTransition> t)
{
    if (!t) return;
    if (t->from) {
        if (t->from->video) t->from->video->close();
        releaseAudio(*m_audio, t->from->audio);
    }
    auto holder = std::make_shared<std::unique_ptr<SourceTransition>>(std::move(t));
    runGl(
        [this, holder] {
            SourceTransition &x = **holder;
            if (x.from) releaseLayer(*x.from);
            if (x.shader) x.shader->releaseGl();
            x.target.destroy();
        },
        false);
}

// ---------------------------------------------------------------------------
// Project file: paths also relative to the project, thumbnail as PNG

QJsonObject Engine::snapshotToJson(const Snapshot &m, const QString &dir) const
{
    QJsonArray layers;
    for (const QJsonValue &v : m.layers) {
        QJsonObject o = v.toObject();
        auto rel = [&](QJsonObject x) {
            const QString p = x.value("path").toString();
            if (!p.isEmpty() && !dir.isEmpty()) x["relative_path"] = QDir(dir).relativeFilePath(p);
            return x;
        };
        o["source"] = rel(o.value("source").toObject());
        QJsonArray fx;
        for (const QJsonValue &e : o.value("fx").toArray()) fx.append(rel(e.toObject()));
        o["fx"] = fx;
        layers.append(o);
    }
    QJsonObject out{{"id", QString::number(m.id)}, {"name", m.name}, {"fade", m.fade}, {"layers", layers}};
    if (!m.composition.isEmpty()) out["composition"] = m.composition;
    if (!m.thumbnail.isNull()) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        m.thumbnail.save(&buf, "PNG");
        out["thumbnail"] = QString::fromLatin1(png.toBase64());
    }
    return out;
}

Engine::Snapshot Engine::snapshotFromJson(const QJsonObject &o, const QString &dir) const
{
    Snapshot m;
    m.id = o.value("id").toString().toULongLong(); // addSnapshot gives one when missing or taken
    m.name = o.value("name").toString();
    m.fade = std::clamp(o.value("fade").toDouble(1.0), 0.0, 600.0);
    m.thumbnail.loadFromData(QByteArray::fromBase64(o.value("thumbnail").toString().toLatin1()), "PNG");
    m.composition = o.value("composition").toObject();
    for (const QJsonValue &v : o.value("layers").toArray()) {
        QJsonObject l = v.toObject();
        auto resolve = [&](QJsonObject x) {
            if (x.contains("path")) x["path"] = resolvePath(x, dir);
            x.remove("relative_path");
            return x;
        };
        QJsonObject src = l.value("source").toObject();
        if (src.value("type").toString() != "none") src = resolve(src);
        l["source"] = src;
        QJsonArray fx;
        for (const QJsonValue &e : l.value("fx").toArray()) fx.append(resolve(e.toObject()));
        l["fx"] = fx;
        m.layers.append(l);
    }
    return m;
}
