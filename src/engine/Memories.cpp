// Memories (cues): snapshots of the layers, recalled with a fade; another source comes in with a transition.
#include "EngineInternal.h"

#include <QBuffer>
#include <QDir>
#include <QJsonArray>
#include <QSet>

// Easing curve types for parameter interpolation
enum class EasingCurve { Linear, EaseIn, EaseOut, EaseInOut, EaseInCubic, EaseOutCubic };

QString easingCurveKey(EasingCurve c)
{
    switch (c) {
    case EasingCurve::Linear: return QStringLiteral("linear");
    case EasingCurve::EaseIn: return QStringLiteral("easeIn");
    case EasingCurve::EaseOut: return QStringLiteral("easeOut");
    case EasingCurve::EaseInOut: return QStringLiteral("easeInOut");
    case EasingCurve::EaseInCubic: return QStringLiteral("easeInCubic");
    case EasingCurve::EaseOutCubic: return QStringLiteral("easeOutCubic");
    }
    return QStringLiteral("easeInOut");
}

EasingCurve easingCurveFromKey(const QString &k)
{
    if (k == "linear") return EasingCurve::Linear;
    if (k == "easeIn") return EasingCurve::EaseIn;
    if (k == "easeOut") return EasingCurve::EaseOut;
    if (k == "easeInOut") return EasingCurve::EaseInOut;
    if (k == "easeInCubic") return EasingCurve::EaseInCubic;
    if (k == "easeOutCubic") return EasingCurve::EaseOutCubic;
    return EasingCurve::EaseInOut; // default
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

// Numbers of a layer that fade from one memory to the next
struct LayerNumbers {
    float opacity = 1, volume = 1;
    QRectF roi;
    ColorAdjust color;
    Mapping mapping;
    std::vector<std::vector<IsfValue>> isf; // [0] generator, [1 + k] effect k
    std::map<quint64, float> viewportOpacity; // per-viewport opacity (0..1)
    SoftEdge soft; // crop feathering (width and power per side)
    double speed = 1.0;
    double inPoint = 0, outPoint = -1;
    double textAnimation = 0.0; // text layer animation parameter (0..1)
};

// How long each of those numbers takes to reach the memory's value (seconds; 0: a cut). By default the
// memory's fade; a memory can give any of them a time of its own ("timing" in its layer state).
struct LayerTimes {
    double opacity = 0, volume = 0, roi = 0, temp = 0, tint = 0, add = 0, remove = 0, mapping = 0;
    std::vector<std::vector<double>> isf; // as LayerNumbers::isf
    double viewportOpacity = 0; // viewport opacity per-viewport
    double softEdge = 0; // soft edge width and power
    double speed = 0, inPoint = 0, outPoint = 0; // playback parameters
    double textAnimation = 0; // text animation parameter
    // Easing curves for each parameter (default: EaseInOut for all)
    EasingCurve opacityCurve = EasingCurve::EaseInOut, volumeCurve = EasingCurve::EaseInOut;
    EasingCurve roiCurve = EasingCurve::EaseInOut, colorCurve = EasingCurve::EaseInOut;
    EasingCurve mappingCurve = EasingCurve::EaseInOut, softEdgeCurve = EasingCurve::EaseInOut;
    EasingCurve viewportOpacityCurve = EasingCurve::EaseInOut;
    EasingCurve speedCurve = EasingCurve::EaseInOut, inOutCurve = EasingCurve::EaseInOut;
    EasingCurve textAnimationCurve = EasingCurve::EaseInOut;
    std::vector<std::vector<EasingCurve>> isfCurves; // per-parameter curves

    double longest() const
    {
        double m = std::max({opacity, volume, roi, temp, tint, add, remove, mapping, viewportOpacity, softEdge, speed, inPoint, outPoint, textAnimation});
        for (const auto &v : isf)
            for (double d : v) m = std::max(m, d);
        return m;
    }
};

struct Engine::FadeJob {
    quint64 id = 0;
    LayerNumbers from, to;
    LayerTimes times;
    bool hideAtEnd = false;
    float finalOpacity = 1;
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
    n.speed = l.speed;
    n.inPoint = l.inPoint;
    n.outPoint = l.outPoint;
    n.textAnimation = l.textAnimation;
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

static void setNumbers(Layer &l, const LayerNumbers &n)
{
    l.opacity = n.opacity;
    l.volume = n.volume;
    l.roi = n.roi;
    l.color = n.color;
    const unsigned rev = l.mapping.revision;
    l.mapping = n.mapping;
    l.mapping.soft = n.soft;
    l.mapping.revision = rev + 1;
    l.viewportOpacity = n.viewportOpacity;
    l.speed = n.speed;
    l.inPoint = n.inPoint;
    l.outPoint = n.outPoint;
    l.textAnimation = n.textAnimation;
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
        n.inPoint = mixd(a.inPoint, b.inPoint, t(d.inPoint, d.inOutCurve));
        n.outPoint = mixd(a.outPoint, b.outPoint, t(d.outPoint, d.inOutCurve));
    }
    // Text animation parameter
    {
        n.textAnimation = mixd(a.textAnimation, b.textAnimation, t(d.textAnimation, d.textAnimationCurve));
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

// Times of a layer's numbers: the memory's fade, or the time the memory gives that value (key → seconds)
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
    d.volume = time(QStringLiteral("volume"));
    d.roi = time(QStringLiteral("roi"));
    d.temp = time(QStringLiteral("color/temp"));
    d.tint = time(QStringLiteral("color/tint"));
    d.add = time(QStringLiteral("color/add"));
    d.remove = time(QStringLiteral("color/remove"));
    d.mapping = time(QStringLiteral("mapping"));
    d.viewportOpacity = time(QStringLiteral("viewportOpacity"));
    d.softEdge = time(QStringLiteral("softEdge"));
    d.speed = time(QStringLiteral("speed"));
    d.inPoint = time(QStringLiteral("inPoint"));
    d.outPoint = time(QStringLiteral("outPoint"));
    d.textAnimation = time(QStringLiteral("textAnimation"));
    // Read easing curves for each parameter
    d.opacityCurve = curve(QStringLiteral("opacity"));
    d.volumeCurve = curve(QStringLiteral("volume"));
    d.roiCurve = curve(QStringLiteral("roi"));
    d.colorCurve = curve(QStringLiteral("color/temp")); // use temp for all color parameters
    d.mappingCurve = curve(QStringLiteral("mapping"));
    d.softEdgeCurve = curve(QStringLiteral("softEdge"));
    d.viewportOpacityCurve = curve(QStringLiteral("viewportOpacity"));
    d.speedCurve = curve(QStringLiteral("speed"));
    d.inOutCurve = curve(QStringLiteral("inPoint")); // use inPoint for both inPoint and outPoint
    d.textAnimationCurve = curve(QStringLiteral("textAnimation"));
    auto params = [&](const IsfInstance *inst, const QString &base) {
        std::vector<double> v;
        if (inst)
            for (const IsfInput &in : inst->inputs()) v.push_back(time(base + in.name));
        return v;
    };
    d.isf.push_back(params(l.generator.get(), QStringLiteral("source/params/")));
    for (size_t k = 0; k < l.effects.size(); ++k)
        d.isf.push_back(params(l.effects[k].get(), QStringLiteral("effects/%1/params/").arg(k)));
    return d;
}

QString Engine::timingKey(const QStringList &path)
{
    if (path.isEmpty()) return {};
    const QString &a = path[0];
    if (a == "opacity" || a == "volume" || a == "mapping" || a == "viewportOpacity" || a == "softEdge" ||
        a == "speed" || a == "inPoint" || a == "outPoint" || a == "textAnimation")
        return a;
    if (a == "source" && path.size() >= 2 && path[1] == "roi") return QStringLiteral("roi");
    if (a == "source" && path.size() >= 3 && path[1] == "params") return QStringLiteral("source/params/") + path[2];
    if (a == "color" && path.size() >= 2 && (path[1] == "temp" || path[1] == "tint" || path[1] == "add" || path[1] == "remove"))
        return QStringLiteral("color/") + path[1];
    if (a == "effects" && path.size() >= 4 && path[2] == "params")
        return QStringLiteral("effects/%1/params/%2").arg(path[1], path[3]);
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

int Engine::memoryCount() const
{
    Lock lk(&m_mutex);
    return int(m_memories.size());
}

Engine::Memory Engine::memory(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_memories.size()) ? m_memories[size_t(i)] : Memory();
}

int Engine::indexOfMemory(quint64 id) const
{
    Lock lk(&m_mutex);
    for (size_t i = 0; i < m_memories.size(); ++i)
        if (m_memories[i].id == id) return int(i);
    return -1;
}

void Engine::setMemory(int i, const Memory &m)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_memories.size())) return;
        const quint64 id = m_memories[size_t(i)].id; // it keeps its identity
        m_memories[size_t(i)] = m;
        m_memories[size_t(i)].id = id;
    }
    emit memoriesChanged();
}

int Engine::addMemory(const Memory &m, int at)
{
    {
        Lock lk(&m_mutex);
        if (at < 0 || at > int(m_memories.size())) at = int(m_memories.size());
        Memory copy = m;
        bool taken = !copy.id;
        for (const Memory &o : m_memories) taken = taken || o.id == copy.id;
        if (taken) copy.id = m_nextMemoryId++;
        m_nextMemoryId = std::max(m_nextMemoryId, copy.id + 1);
        m_memories.insert(m_memories.begin() + at, copy);
    }
    emit memoriesChanged();
    return at;
}

void Engine::removeMemory(int i)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_memories.size())) return;
        m_memories.erase(m_memories.begin() + i);
    }
    emit memoriesChanged();
}

QJsonArray Engine::captureLayers() const
{
    Lock lk(&m_mutex);
    QJsonArray a;
    for (int i = 0; i < int(m_layers.size()); ++i) {
        if (m_layers[size_t(i)]->isViewport) continue; // a viewport has one state, not one per memory
        QJsonObject o = layerJson(i);
        o["included"] = true;
        a.append(o);
    }
    return a;
}

Engine::RecallProgress Engine::recallProgress() const
{
    Lock lk(&m_mutex);
    RecallProgress r;
    r.memory = m_recalledMemory;
    r.total = m_recallTotal;
    r.elapsed = m_fades.empty() ? m_recallTotal : m_fadeElapsed;
    return r;
}

bool Engine::isFading() const
{
    Lock lk(&m_mutex);
    return !m_fades.empty();
}

void Engine::recallMemory(int i)
{
    const Memory m = memory(i);
    if (m.layers.isEmpty()) return;
    applyLayers(m.layers, m.fade, true);
    {
        Lock lk(&m_mutex);
        m_recalledMemory = m.id;
        double total = 0;
        for (const auto &job : m_fades) total = std::max(total, job->times.longest());
        for (const auto &[id, t] : m_transitions) total = std::max(total, t->duration - t->elapsed);
        m_recallTotal = total;
    }
    emit memoryRecalled(i);
}

void Engine::applyLayers(const QJsonArray &layers, double fade, bool hideOthers)
{
    QSet<quint64> named; // the layers the state speaks of (left out or not)
    for (const QJsonValue &v : layers) named.insert(v.toObject().value("id").toString().toULongLong());
    {
        Lock lk(&m_mutex);
        m_fades.clear(); // a new recall takes over from where the previous one is
    }
    std::vector<std::shared_ptr<FadeJob>> jobs;
    for (const QJsonValue &value : layers) {
        const QJsonObject o = value.toObject();
        if (!o.value("included").toBool(true) || o.value("viewport").toBool()) continue;
        const quint64 id = o.value("id").toString().toULongLong();
        int idx = indexOfId(id);
        if (idx >= 0 && isLocked(idx)) continue; // a locked layer is not changed by a memory
        if (idx < 0) { // removed since: recreated at the bottom
            insertLayerJson(layerCount(), o);
            continue;
        }
        const QJsonObject cur = layerJson(idx);
        const QJsonObject curSrc = cur.value("source").toObject(), src = o.value("source").toObject();
        const bool group = o.value("group").toBool();
        if (!group && (curSrc.value("type") != src.value("type") || curSrc.value("layer") != src.value("layer") ||
                       curSrc.value("tap") != src.value("tap") ||
                       QDir::cleanPath(curSrc.value("path").toString()) != QDir::cleanPath(src.value("path").toString()))) {
            // Another media: it comes in with the layer's transition over the source's time (the memory's
            // fade unless it has its own), or at once for a cut
            const QJsonValue own = o.value("timing").toObject().value("source");
            const double t = own.isDouble() ? std::clamp(own.toDouble(), 0.0, 600.0) : std::max(0.0, fade);
            if (t <= 0) {
                replaceLayerJson(idx, o);
                continue;
            }
            startSourceTransition(idx, o, t);
            // The layer now holds the memory's state; its numbers move there from the outgoing one's (ROI,
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
            job->from.opacity = old.visible ? old.opacity : 0.0f;
            job->from.volume = old.volume;
            job->from.roi = old.roi;
            job->from.color = old.color;
            if (old.mapping.cols == l->mapping.cols && old.mapping.rows == l->mapping.rows) job->from.mapping = old.mapping;
            job->times = timesOf(*l, o.value("timing").toObject(), std::max(0.0, fade));
            setNumbers(*l, mixNumbers(job->from, job->to, job->times, 0));
            jobs.push_back(job);
            continue;
        }
        if (effectPaths(cur.value("effects").toArray()) != effectPaths(o.value("effects").toArray()))
            setEffectsJson(idx, o.value("effects").toArray()); // another chain: at once
        if (src.contains("playMode")) {
            setLayerPlayMode(idx, playModeFromKey(src.value("playMode").toString()));
            setLayerSpeed(idx, src.value("speed").toDouble(1.0));
            setLayerInOut(idx, src.value("in").toDouble(0), src.value("out").toDouble(-1));
        }

        Lock lk(&m_mutex);
        Layer *l = layer(idx);
        if (!l) continue;
        l->name = o.value("name").toString(l->name);
        l->blend = blendModeFromKey(o.value("blend").toString(blendModeKey(l->blend)));
        l->effectsEnabled = o.value("effectsEnabled").toBool(l->effectsEnabled);
        l->muted = o.value("muted").toBool(l->muted);
        // Which viewports it is drawn in: a memory can send a layer to another projector
        l->viewportOpacity.clear();
        const QJsonObject vo = o.value("viewportOpacity").toObject();
        for (auto it = vo.begin(); it != vo.end(); ++it)
            l->viewportOpacity[it.key().toULongLong()] = float(std::clamp(it.value().toDouble(1.0), 0.0, 1.0));
        const QJsonArray fx = o.value("effects").toArray();
        for (size_t k = 0; k < l->effects.size() && int(k) < fx.size(); ++k)
            l->effects[k]->readState(fx[int(k)].toObject()); // on, mask
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
        const QJsonArray roi = src.value("roi").toArray();
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
        if (o.contains("mapping")) to.mapping.fromJson(o.value("mapping").toObject());
        if (!to.isf.empty()) readParams(l->generator.get(), src.value("params").toObject(), to.isf[0]);
        for (size_t k = 0; k < l->effects.size() && k + 1 < to.isf.size() && int(k) < fx.size(); ++k)
            readParams(l->effects[k].get(), fx[int(k)].toObject().value("params").toObject(), to.isf[k + 1]);

        // Visibility: shown at once and faded in from 0, or faded out then hidden
        const bool visible = o.value("visible").toBool(true);
        if (visible && !l->visible) {
            l->visible = true;
            job->from.opacity = 0;
        } else if (!visible && l->visible) {
            job->hideAtEnd = true;
            job->finalOpacity = to.opacity;
            to.opacity = 0;
        }
        if (job->times.longest() <= 0) {
            setNumbers(*l, to);
            if (job->hideAtEnd) {
                l->visible = false;
                l->opacity = job->finalOpacity;
            }
        } else {
            // Bools and lists reach their target at once; the numbers start from where they are (a cut: there)
            setNumbers(*l, mixNumbers(job->from, to, job->times, 0));
            if (job->hideAtEnd && job->times.opacity <= 0) { // hidden by a cut
                l->visible = false;
                l->opacity = job->finalOpacity;
                job->hideAtEnd = false;
                job->from.opacity = job->to.opacity = job->finalOpacity;
            }
            jobs.push_back(job);
        }
    }
    fixLayerReferences(); // layers re-created or sources changed: no reference left dangling or looping
    Lock lk(&m_mutex);
    // The layers the memory does not know (created since): faded out with the memory's fade, then hidden
    if (hideOthers)
        for (auto &lp : m_layers) {
            Layer &l = *lp;
            if (l.isViewport || named.contains(l.id) || !l.visible || isLocked(indexOfId(l.id))) continue;
            if (fade <= 0) {
                l.visible = false;
                continue;
            }
            auto job = std::make_shared<FadeJob>();
            job->id = l.id;
            job->from = numbersOf(l);
            job->to = job->from;
            job->to.opacity = 0;
            job->finalOpacity = l.opacity;
            job->hideAtEnd = true;
            job->times = timesOf(l, QJsonObject(), fade);
            jobs.push_back(job);
        }
    m_fades = std::move(jobs);
    m_fadeElapsed = 0;
}

// Every number moves on its own time; a layer that fades out is hidden once its opacity got there
void Engine::stepFade(double dt)
{
    if (m_fades.empty()) return;
    m_fadeElapsed += std::max(0.0, dt);
    bool running = false;
    for (const auto &job : m_fades) {
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == job->id) l = x.get();
        if (!l) continue;
        setNumbers(*l, mixNumbers(job->from, job->to, job->times, m_fadeElapsed));
        if (job->hideAtEnd && m_fadeElapsed >= job->times.opacity) {
            l->visible = false;
            l->opacity = job->finalOpacity;
            job->hideAtEnd = false;
            job->from.opacity = job->to.opacity = job->finalOpacity; // stays as stored while the rest moves on
        }
        running = running || m_fadeElapsed < job->times.longest();
    }
    if (!running) m_fades.clear();
}

void Engine::advanceFades(double dt)
{
    Lock lk(&m_mutex);
    stepFade(dt);
    stepTransitions(dt);
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
        t.from->visible = l->visible; // heard as the layer is
        t.from->parentVisible = l->parentVisible;
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

QJsonObject Engine::memoryToJson(const Memory &m, const QString &dir) const
{
    QJsonArray layers;
    for (const QJsonValue &v : m.layers) {
        QJsonObject o = v.toObject();
        auto rel = [&](QJsonObject x) {
            const QString p = x.value("path").toString();
            if (!p.isEmpty() && !dir.isEmpty()) x["relativePath"] = QDir(dir).relativeFilePath(p);
            return x;
        };
        o["source"] = rel(o.value("source").toObject());
        QJsonArray fx;
        for (const QJsonValue &e : o.value("effects").toArray()) fx.append(rel(e.toObject()));
        o["effects"] = fx;
        layers.append(o);
    }
    QJsonObject out{{"id", QString::number(m.id)}, {"name", m.name}, {"fade", m.fade}, {"layers", layers}};
    if (!m.thumbnail.isNull()) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        m.thumbnail.save(&buf, "PNG");
        out["thumbnail"] = QString::fromLatin1(png.toBase64());
    }
    return out;
}

Engine::Memory Engine::memoryFromJson(const QJsonObject &o, const QString &dir) const
{
    Memory m;
    m.id = o.value("id").toString().toULongLong(); // addMemory gives one when missing or taken
    m.name = o.value("name").toString();
    m.fade = std::clamp(o.value("fade").toDouble(1.0), 0.0, 600.0);
    m.thumbnail.loadFromData(QByteArray::fromBase64(o.value("thumbnail").toString().toLatin1()), "PNG");
    for (const QJsonValue &v : o.value("layers").toArray()) {
        QJsonObject l = v.toObject();
        auto resolve = [&](QJsonObject x) {
            if (x.contains("path")) x["path"] = resolvePath(x, dir);
            x.remove("relativePath");
            return x;
        };
        QJsonObject src = l.value("source").toObject();
        if (src.value("type").toString() != "none") src = resolve(src);
        l["source"] = src;
        QJsonArray fx;
        for (const QJsonValue &e : l.value("effects").toArray()) fx.append(resolve(e.toObject()));
        l["effects"] = fx;
        m.layers.append(l);
    }
    return m;
}
