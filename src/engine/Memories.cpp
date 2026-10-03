// Memories (cues): snapshots of the layers, recalled with a fade.
#include "Engine.h"

#include <QBuffer>
#include <QDir>
#include <QJsonArray>

// Numbers of a layer that fade from one memory to the next
struct LayerNumbers {
    float opacity = 1, volume = 1;
    QRectF crop;
    ColorAdjust color;
    Mapping mapping;
    std::vector<std::vector<IsfValue>> isf; // [0] generator, [1 + k] effect k
};

struct Engine::FadeJob {
    quint64 id = 0;
    LayerNumbers from, to;
    bool hideAtEnd = false;
    float finalOpacity = 1;
};

static LayerNumbers numbersOf(const Layer &l)
{
    LayerNumbers n;
    n.opacity = l.opacity;
    n.volume = l.volume;
    n.crop = l.crop;
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
    l.crop = n.crop;
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

static LayerNumbers mixNumbers(const LayerNumbers &a, const LayerNumbers &b, double t)
{
    LayerNumbers n = b;
    n.opacity = mixf(a.opacity, b.opacity, t);
    n.volume = mixf(a.volume, b.volume, t);
    n.crop = QRectF(mixp(a.crop.topLeft(), b.crop.topLeft(), t), mixp(a.crop.bottomRight(), b.crop.bottomRight(), t));
    n.color.temp = mixf(a.color.temp, b.color.temp, t);
    n.color.tint = mixf(a.color.tint, b.color.tint, t);
    for (int c = 0; c < 3; ++c) {
        n.color.add[c] = mixf(a.color.add[c], b.color.add[c], t);
        n.color.remove[c] = mixf(a.color.remove[c], b.color.remove[c], t);
    }
    if (a.mapping.cols == b.mapping.cols && a.mapping.rows == b.mapping.rows) {
        for (int k = 0; k < 4; ++k) n.mapping.corners[k] = mixp(a.mapping.corners[k], b.mapping.corners[k], t);
        for (size_t k = 0; k < n.mapping.offsets.size() && k < a.mapping.offsets.size(); ++k)
            n.mapping.offsets[k] = mixp(a.mapping.offsets[k], b.mapping.offsets[k], t);
    }
    for (size_t i = 0; i < n.isf.size() && i < a.isf.size(); ++i)
        for (size_t k = 0; k < n.isf[i].size() && k < a.isf[i].size(); ++k) {
            IsfValue &v = n.isf[i][k];
            const IsfValue &f = a.isf[i][k];
            v.f = mixd(f.f, v.f, t);
            v.p = mixp(f.p, v.p, t);
            for (int c = 0; c < 4; ++c) v.c[c] = mixf(f.c[c], v.c[c], t);
            // bools and lists: the target, at once
        }
    return n;
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
        if (!o.value("included").toBool(true)) continue;
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
        if (!group && (curSrc.value("type") != src.value("type") ||
                       QDir::cleanPath(curSrc.value("path").toString()) != QDir::cleanPath(src.value("path").toString()))) {
            replaceLayerJson(idx, o); // another media: loaded at once, no fade
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
        const QJsonArray fx = o.value("effects").toArray();
        for (size_t k = 0; k < l->effects.size() && int(k) < fx.size(); ++k)
            l->effects[k]->enabled = fx[int(k)].toObject().value("enabled").toBool(true);

        auto job = std::make_shared<FadeJob>();
        job->id = id;
        job->from = numbersOf(*l);
        LayerNumbers &to = job->to;
        to = job->from;
        to.opacity = float(o.value("opacity").toDouble(to.opacity));
        to.volume = float(std::clamp(o.value("volume").toDouble(to.volume), 0.0, 2.0));
        const QJsonArray crop = src.value("crop").toArray();
        if (crop.size() == 4)
            to.crop = QRectF(QPointF(crop[0].toDouble(), crop[1].toDouble()), QPointF(crop[2].toDouble(), crop[3].toDouble()));
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
        if (fade <= 0) {
            setNumbers(*l, to);
            if (job->hideAtEnd) {
                l->visible = false;
                l->opacity = job->finalOpacity;
            }
        } else {
            // Bools and lists reach their target at once; the numbers start from where they are
            setNumbers(*l, mixNumbers(job->from, to, 0));
            jobs.push_back(job);
        }
    }
    Lock lk(&m_mutex);
    m_fades = std::move(jobs);
    m_fadeT = 0;
    m_fadeDuration = std::max(0.0, fade);
}

void Engine::stepFade(double dt)
{
    if (m_fades.empty()) return;
    m_fadeT = m_fadeDuration > 0 ? std::min(1.0, m_fadeT + dt / m_fadeDuration) : 1.0;
    const double t = m_fadeT * m_fadeT * (3 - 2 * m_fadeT); // smooth start and end
    for (const auto &job : m_fades) {
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == job->id) l = x.get();
        if (!l) continue;
        setNumbers(*l, mixNumbers(job->from, job->to, t));
        if (m_fadeT >= 1.0 && job->hideAtEnd) {
            l->visible = false;
            l->opacity = job->finalOpacity;
        }
    }
    if (m_fadeT >= 1.0) m_fades.clear();
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
