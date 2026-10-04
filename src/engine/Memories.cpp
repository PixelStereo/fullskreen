// Memories (cues): snapshots of the layers, recalled with a fade; another source comes in with a transition.
#include "EngineInternal.h"

#include <QBuffer>
#include <QDir>
#include <QJsonArray>

// Numbers of a layer that fade from one memory to the next
struct LayerNumbers {
    float opacity = 1, volume = 1;
    QRectF roi;
    ColorAdjust color;
    Mapping mapping;
    std::vector<std::vector<IsfValue>> isf; // [0] generator, [1 + k] effect k
};

// How long each of those numbers takes to reach the memory's value (seconds; 0: a cut). By default the
// memory's fade; a memory can give any of them a time of its own ("timing" in its layer state).
struct LayerTimes {
    double opacity = 0, volume = 0, roi = 0, temp = 0, tint = 0, add = 0, remove = 0, mapping = 0;
    std::vector<std::vector<double>> isf; // as LayerNumbers::isf

    double longest() const
    {
        double m = std::max({opacity, volume, roi, temp, tint, add, remove, mapping});
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
    l.mapping.revision = rev + 1;
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

// Progress of a number `elapsed` seconds into its own time: 1 for a cut, eased at both ends otherwise
static double progress(double elapsed, double duration)
{
    if (duration <= 0) return 1.0;
    const double t = std::min(1.0, elapsed / duration);
    return t * t * (3 - 2 * t);
}

static LayerNumbers mixNumbers(const LayerNumbers &a, const LayerNumbers &b, const LayerTimes &d, double elapsed)
{
    auto t = [elapsed](double duration) { return progress(elapsed, duration); };
    LayerNumbers n = b;
    n.opacity = mixf(a.opacity, b.opacity, t(d.opacity));
    n.volume = mixf(a.volume, b.volume, t(d.volume));
    const double tr = t(d.roi);
    n.roi = QRectF(mixp(a.roi.topLeft(), b.roi.topLeft(), tr), mixp(a.roi.bottomRight(), b.roi.bottomRight(), tr));
    n.color.temp = mixf(a.color.temp, b.color.temp, t(d.temp));
    n.color.tint = mixf(a.color.tint, b.color.tint, t(d.tint));
    for (int c = 0; c < 3; ++c) {
        n.color.add[c] = mixf(a.color.add[c], b.color.add[c], t(d.add));
        n.color.remove[c] = mixf(a.color.remove[c], b.color.remove[c], t(d.remove));
    }
    if (a.mapping.cols == b.mapping.cols && a.mapping.rows == b.mapping.rows) {
        const double tm = t(d.mapping);
        for (int k = 0; k < 4; ++k) n.mapping.corners[k] = mixp(a.mapping.corners[k], b.mapping.corners[k], tm);
        for (size_t k = 0; k < n.mapping.offsets.size() && k < a.mapping.offsets.size(); ++k)
            n.mapping.offsets[k] = mixp(a.mapping.offsets[k], b.mapping.offsets[k], tm);
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
    LayerTimes d;
    d.opacity = time(QStringLiteral("opacity"));
    d.volume = time(QStringLiteral("volume"));
    d.roi = time(QStringLiteral("roi"));
    d.temp = time(QStringLiteral("color/temp"));
    d.tint = time(QStringLiteral("color/tint"));
    d.add = time(QStringLiteral("color/add"));
    d.remove = time(QStringLiteral("color/remove"));
    d.mapping = time(QStringLiteral("mapping"));
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
    if (a == "opacity" || a == "volume" || a == "mapping") return a;
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

void Engine::setMemory(int i, const Memory &m)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_memories.size())) return;
        m_memories[size_t(i)] = m;
    }
    emit memoriesChanged();
}

int Engine::addMemory(const Memory &m, int at)
{
    {
        Lock lk(&m_mutex);
        if (at < 0 || at > int(m_memories.size())) at = int(m_memories.size());
        m_memories.insert(m_memories.begin() + at, m);
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

bool Engine::isFading() const
{
    Lock lk(&m_mutex);
    return !m_fades.empty();
}

void Engine::recallMemory(int i)
{
    const Memory m = memory(i);
    if (m.layers.isEmpty()) return;
    applyLayers(m.layers, m.fade);
    emit memoryRecalled(i);
}

void Engine::applyLayers(const QJsonArray &layers, double fade)
{
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
            if (t > 0) startSourceTransition(idx, o, t);
            else replaceLayerJson(idx, o);
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
        l->hiddenIn.clear();
        for (const QJsonValue &v : o.value("hiddenIn").toArray()) l->hiddenIn.push_back(v.toString().toULongLong());
        const QJsonArray fx = o.value("effects").toArray();
        for (size_t k = 0; k < l->effects.size() && int(k) < fx.size(); ++k)
            l->effects[k]->readState(fx[int(k)].toObject()); // on, mask

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
    QJsonObject out{{"name", m.name}, {"fade", m.fade}, {"layers", layers}};
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
