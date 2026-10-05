// Timelines (animations): numbers of the layers drawn over time — curves and oscillators — played on their own
// clock; the sequences drive their transport.
#include "EngineInternal.h"

#include <QJsonArray>
#include <cmath>
#include <limits>

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kMinDuration = 0.05;

// ---------------------------------------------------------------------------
// Values

double Engine::AnimTrack::valueAt(double position, double played) const
{
    if (oscillator) {
        const double per = std::max(0.01, period);
        double p = played / per + phase;
        p -= std::floor(p); // 0..1 within the period
        double w = 0;
        switch (wave) {
        case AnimWave::Sine: w = std::sin(2 * kPi * p); break;
        case AnimWave::Triangle: w = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4; break; // starts at 0, rises
        case AnimWave::Saw: w = 2 * p - 1; break;
        case AnimWave::Square: w = p < 0.5 ? 1 : -1; break;
        }
        return center + amplitude * w;
    }
    if (keys.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (position <= keys.front().t) return keys.front().v;
    if (position >= keys.back().t) return keys.back().v;
    for (size_t k = 0; k + 1 < keys.size(); ++k) {
        const AnimKey &a = keys[k], &b = keys[k + 1];
        if (position >= b.t) continue;
        if (a.curve == kAnimHold || b.t - a.t <= 1e-9) return a.v;
        return a.v + (b.v - a.v) * easeCurve((position - a.t) / (b.t - a.t), a.curve);
    }
    return keys.back().v;
}

static double passDuration(double d) { return std::max(kMinDuration, d); }

double Engine::Animation::length() const
{
    const double d = passDuration(duration);
    if (loop == AnimLoop::Once) return d;
    return repeat > 0 ? d * repeat : std::numeric_limits<double>::infinity();
}

double Engine::Animation::position(double c) const
{
    const double d = passDuration(duration);
    c = std::clamp(c, 0.0, length());
    if (loop == AnimLoop::Once) return c;
    double pass = std::floor(c / d);
    double within = c - pass * d;
    if (std::isfinite(length()) && c >= length()) { // the end of the last pass
        pass = repeat - 1;
        within = d;
    }
    const bool backwards = loop == AnimLoop::PingPong && std::fmod(pass, 2.0) == 1.0;
    return backwards ? d - within : within;
}

// ---------------------------------------------------------------------------
// The list

int Engine::animationCount() const
{
    Lock lk(&m_mutex);
    return int(m_animations.size());
}

Engine::Animation Engine::animation(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_animations.size()) ? m_animations[size_t(i)] : Animation();
}

int Engine::indexOfAnimation(quint64 id) const
{
    Lock lk(&m_mutex);
    for (size_t i = 0; i < m_animations.size(); ++i)
        if (m_animations[i].id == id) return int(i);
    return -1;
}

static void sortKeys(Engine::AnimTrack &t)
{
    std::stable_sort(t.keys.begin(), t.keys.end(), [](const Engine::AnimKey &a, const Engine::AnimKey &b) { return a.t < b.t; });
}

int Engine::addAnimation(const Animation &a, int at)
{
    {
        Lock lk(&m_mutex);
        Animation x = a;
        bool taken = !x.id;
        for (const Animation &o : m_animations) taken = taken || o.id == x.id;
        if (taken) x.id = m_nextAnimationId;
        m_nextAnimationId = std::max(m_nextAnimationId, x.id + 1);
        x.state = AnimState::Stopped;
        x.clock = 0;
        for (AnimTrack &t : x.tracks) sortKeys(t);
        if (at < 0 || at > int(m_animations.size())) at = int(m_animations.size());
        m_animations.insert(m_animations.begin() + at, x);
    }
    emit animationsChanged();
    return at;
}

void Engine::setAnimation(int i, const Animation &a)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_animations.size())) return;
        Animation &x = m_animations[size_t(i)];
        const AnimState state = x.state;
        const double clock = x.clock;
        const quint64 id = x.id;
        x = a;
        x.id = id;
        x.state = state;
        x.clock = std::min(clock, x.length());
        for (AnimTrack &t : x.tracks) sortKeys(t);
    }
    emit animationsChanged();
}

void Engine::removeAnimation(int i)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_animations.size())) return;
        m_animations.erase(m_animations.begin() + i);
    }
    emit animationsChanged();
}

// ---------------------------------------------------------------------------
// Transport

void Engine::controlAnimation(quint64 id, AnimAction action, double time, AnimLoop loop, int repeat)
{
    {
        Lock lk(&m_mutex);
        Animation *a = nullptr;
        for (Animation &x : m_animations)
            if (x.id == id) a = &x;
        if (!a) return;
        controlLocked(a, action, time, loop, repeat);
    }
    if (action == AnimAction::LoopMode) emit animationsChanged(); // its definition (saved with it)
}

void Engine::controlLocked(Animation *a, AnimAction action, double time, AnimLoop loop, int repeat)
{
    switch (action) {
    case AnimAction::Play:
        if (a->state == AnimState::Stopped) a->clock = 0;
        a->state = AnimState::Playing;
        applyAnimation(*a); // its first values now, not a frame later
        break;
    case AnimAction::Pause:
        if (a->state == AnimState::Playing) a->state = AnimState::Paused;
        break;
    case AnimAction::Stop:
        a->state = AnimState::Stopped;
        a->clock = 0;
        break;
    case AnimAction::Rewind:
        a->clock = 0;
        applyAnimation(*a);
        break;
    case AnimAction::Seek:
        a->clock = std::clamp(time, 0.0, std::min(a->length(), 1e9));
        if (a->state == AnimState::Stopped) a->state = AnimState::Paused; // stays where it was sought
        applyAnimation(*a);
        break;
    case AnimAction::LoopMode:
        a->loop = loop;
        a->repeat = std::max(0, repeat);
        // Don't clamp a->clock to the new length: let position() handle it.
        // If a timeline is looping and we change to Once, it should finish the current
        // loop naturally, not jump to the end.
        break;
    }
}

void Engine::applyAnimation(Animation &a)
{
    const double pos = a.position(), played = std::min(a.clock, a.length());
    for (const AnimTrack &t : a.tracks) {
        if (!t.enabled || t.param.isEmpty()) continue;

        // Handle "current value" keyframe at t=0 (start of timeline)
        if (pos <= 1e-9 && !t.keys.empty() && t.keys.front().t <= 1e-9 && t.keys.front().isCurrentValue) {
            double currentValue = 0;
            if (animParamValue(t.layer, t.param, &currentValue)) {
                setAnimParam(t.layer, t.param, currentValue);
                continue;
            }
        }

        const double v = t.valueAt(pos, played);
        if (std::isfinite(v)) setAnimParam(t.layer, t.param, v);
    }
}

void Engine::stepAnimations(double dt)
{
    dt = std::max(0.0, dt);
    for (Animation &a : m_animations) {
        if (a.state != AnimState::Playing) continue;
        a.clock += dt;
        applyAnimation(a);
        if (a.clock >= a.length()) { // over: its last values stay
            a.state = AnimState::Stopped;
            a.clock = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// The numbers a timeline drives

static double wrapDegrees(double d)
{
    d = std::fmod(d, 360.0);
    if (d <= -180) d += 360;
    if (d > 180) d -= 360;
    return d;
}

// Angle of the mapping's top edge, in degrees, measured in pixels of the composition
static double mappingAngle(const Mapping &m, QSize comp)
{
    const QPointF d = m.corners[1] - m.corners[0];
    return std::atan2(d.y() * comp.height(), d.x() * comp.width()) * 180 / kPi;
}

// Turns the mapping (corners and mesh) around the middle of its bounds, in pixels of the composition
static void rotateMapping(Mapping &m, double degrees, QSize comp)
{
    if (std::abs(degrees) < 1e-9) return;
    const double W = std::max(1, comp.width()), H = std::max(1, comp.height());
    const double a = degrees * kPi / 180, c = std::cos(a), s = std::sin(a);
    const QPointF o = m.bounds().center();
    auto turn = [&](QPointF v) { // a vector in normalized units
        const double x = v.x() * W, y = v.y() * H;
        return QPointF((x * c - y * s) / W, (x * s + y * c) / H);
    };
    for (QPointF &p : m.corners) p = o + turn(p - o);
    for (QPointF &p : m.offsets) p = turn(p);
    ++m.revision;
}

static const IsfInput *findInput(const IsfInstance *inst, const QString &name)
{
    if (!inst) return nullptr;
    for (const IsfInput &in : inst->inputs())
        if (in.name == name) return &in;
    return nullptr;
}

// The number at `path` of layer l: read into *get, or written from *set. False: no such number.
static bool layerParam(Layer &l, const QString &path, double *get, const double *set, QSize comp)
{
    auto num = [&](auto &field, double lo, double hi) {
        if (get) *get = double(field);
        if (set) field = std::remove_reference_t<decltype(field)>(std::clamp(*set, lo, hi));
        return true;
    };
    const QStringList p = path.split(QLatin1Char('/'));
    const QString &a = p.value(0);
    if (p.size() == 1) {
        if (a == "opacity") return num(l.opacity, 0, 1);
        if (a == "volume") return num(l.volume, 0, 2);
        if (a == "speed") return num(l.speed, -16, 16);
        return false;
    }
    if (a == "roi" && p.size() == 2) {
        QRectF r = l.roi;
        double x = r.x(), y = r.y(), w = r.width(), h = r.height();
        bool ok = true;
        if (p[1] == "x") num(x, -1, 2);
        else if (p[1] == "y") num(y, -1, 2);
        else if (p[1] == "w") num(w, 0.001, 4);
        else if (p[1] == "h") num(h, 0.001, 4);
        else ok = false;
        if (ok && set) l.roi = QRectF(x, y, w, h);
        return ok;
    }
    if (a == "color") {
        ColorAdjust &c = l.color;
        if (p[1] == "temp") return num(c.temp, -ColorAdjust::kTempRange, ColorAdjust::kTempRange);
        if (p[1] == "tint") return num(c.tint, -ColorAdjust::kTintRange, ColorAdjust::kTintRange);
        if ((p[1] == "add" || p[1] == "remove") && p.size() == 3) {
            const int k = QStringLiteral("rgb").indexOf(p[2]);
            if (k < 0 || p[2].size() != 1) return false;
            return num(p[1] == "add" ? c.add[k] : c.remove[k], 0, 1);
        }
        return false;
    }
    if (a == "mapping" && p.size() == 2) {
        Mapping &m = l.mapping;
        if (p[1] == "rotation") {
            const double now = mappingAngle(m, comp);
            if (get) *get = now;
            if (set) rotateMapping(m, wrapDegrees(*set - now), comp);
            return true;
        }
        if (p[1] == "x" || p[1] == "y") {
            const QPointF tl = m.bounds().topLeft();
            const double now = p[1] == "x" ? tl.x() : tl.y();
            if (get) *get = now;
            if (set) {
                const double d = std::clamp(*set, -4.0, 5.0) - now;
                m.translate(p[1] == "x" ? QPointF(d, 0) : QPointF(0, d));
            }
            return true;
        }
        return false;
    }
    if (a == "text" && p.size() == 2 && l.isText()) {
        TextSource &t = l.text;
        if (p[1] == "size") {
            double v = t.size;
            num(v, 1, 1000);
            if (set) t.size = int(std::lround(v));
            return true;
        }
        if (p[1] == "lineHeight") return num(t.lineHeight, 0.1, 10);
        if (p[1] == "letterSpacing") return num(t.letterSpacing, -200, 500);
        if (p[1] == "outline") return num(t.outline, 0, 200);
        if (p[1] == "shadowX") return num(t.shadowX, -2000, 2000);
        if (p[1] == "shadowY") return num(t.shadowY, -2000, 2000);
        return false;
    }
    // ISF: source/params/<name>[/<x|y|r|g|b|a>], effects/<k>/params/<name>[/…]
    IsfInstance *inst = nullptr;
    int at = 0;
    if (a == "source" && p.size() >= 3 && p[1] == "params") {
        inst = l.generator.get();
        at = 2;
    } else if (a == "effects" && p.size() >= 4 && p[2] == "params") {
        bool ok = false;
        const int k = p[1].toInt(&ok);
        if (ok && k >= 0 && k < int(l.effects.size())) inst = l.effects[size_t(k)].get();
        at = 3;
    }
    if (!inst) return false;
    IsfInput *in = inst->input(p[at]);
    if (!in) return false;
    const QString part = p.value(at + 1);
    IsfValue v = in->value();
    switch (in->type) {
    case IsfInput::Float:
        if (!part.isEmpty()) return false;
        num(v.f, std::min(in->fMin, in->fMax), std::max(in->fMin, in->fMax));
        break;
    case IsfInput::Long: {
        if (!part.isEmpty()) return false;
        double x = v.l;
        int lo = in->lValues.isEmpty() ? -1000000 : *std::min_element(in->lValues.begin(), in->lValues.end());
        int hi = in->lValues.isEmpty() ? 1000000 : *std::max_element(in->lValues.begin(), in->lValues.end());
        num(x, lo, hi);
        v.l = int(std::lround(x));
        break;
    }
    case IsfInput::Point2D: {
        if (part != "x" && part != "y") return false;
        double x = part == "x" ? v.p.x() : v.p.y();
        num(x, -1e6, 1e6);
        if (part == "x") v.p.setX(x);
        else v.p.setY(x);
        break;
    }
    case IsfInput::Color: {
        const int k = QStringLiteral("rgba").indexOf(part);
        if (k < 0 || part.size() != 1) return false;
        num(v.c[k], 0, 1);
        break;
    }
    default: return false;
    }
    if (set) in->setValue(v);
    return true;
}

bool Engine::setAnimParam(quint64 layer, const QString &path, double v)
{
    if (!layer) {
        if (path == "level") {
            m_masterTarget = std::clamp(v, 0.0, 1.0);
            m_masterSpeed = 0;
            return true;
        }
        if (path == "volume") {
            m_audio->setMasterVolume(float(std::clamp(v, 0.0, 2.0)));
            return true;
        }
        return false;
    }
    for (auto &l : m_layers)
        if (l->id == layer) {
            if (l->locked) return false;
            return layerParam(*l, path, nullptr, &v, m_compSize);
        }
    return false;
}

bool Engine::animParamValue(quint64 layer, const QString &path, double *value) const
{
    Lock lk(&m_mutex);
    if (!layer) {
        if (path == "level") *value = m_masterTarget;
        else if (path == "volume") *value = m_audio->masterVolume();
        else return false;
        return true;
    }
    for (auto &l : m_layers)
        if (l->id == layer) return layerParam(*l, path, value, nullptr, m_compSize);
    return false;
}

std::vector<Engine::AnimParam> Engine::animatableParams(quint64 layer) const
{
    Lock lk(&m_mutex);
    std::vector<AnimParam> out;
    auto add = [&](const QString &path, const QString &label, double lo, double hi) { out.push_back({path, label, lo, hi}); };
    if (!layer) {
        add("level", "Level", 0, 1);
        add("volume", "Volume", 0, 2);
        return out;
    }
    const Layer *l = nullptr;
    for (auto &x : m_layers)
        if (x->id == layer) l = x.get();
    if (!l) return out;
    add("opacity", "Opacity", 0, 1);
    add("mapping/rotation", "Mapping › Rotation (°)", -180, 180);
    add("mapping/x", "Mapping › X", -1, 2);
    add("mapping/y", "Mapping › Y", -1, 2);
    if (l->hasTransport()) {
        add("volume", "Volume", 0, 2);
        add("speed", "Speed", 0, 4);
    }
    if (!l->isGroup) {
        add("roi/x", "ROI › X", 0, 1);
        add("roi/y", "ROI › Y", 0, 1);
        add("roi/w", "ROI › Width", 0, 1);
        add("roi/h", "ROI › Height", 0, 1);
    }
    add("color/temp", "Color › Temperature", -ColorAdjust::kTempRange, ColorAdjust::kTempRange);
    add("color/tint", "Color › Tint", -ColorAdjust::kTintRange, ColorAdjust::kTintRange);
    for (const char *k : {"add", "remove"})
        for (int c = 0; c < 3; ++c)
            add(QStringLiteral("color/%1/%2").arg(QLatin1String(k)).arg(QChar("rgb"[c])),
                QStringLiteral("Color › %1 %2").arg(QLatin1String(k[0] == 'a' ? "Add" : "Remove")).arg(QChar("RGB"[c])), 0, 1);
    if (l->isText()) {
        add("text/size", "Text › Size", 1, 400);
        add("text/lineHeight", "Text › Line height", 0.5, 3);
        add("text/letterSpacing", "Text › Letter spacing", -20, 100);
        add("text/outline", "Text › Outline", 0, 40);
        add("text/shadowX", "Text › Shadow X", -100, 100);
        add("text/shadowY", "Text › Shadow Y", -100, 100);
    }
    auto isf = [&](const IsfInstance *inst, const QString &base, const QString &title) {
        if (!inst) return;
        for (const IsfInput &in : inst->inputs()) {
            const QString label = QStringLiteral("%1 › %2").arg(title, in.label.isEmpty() ? in.name : in.label);
            const QString path = base + in.name;
            switch (in.type) {
            case IsfInput::Float: add(path, label, std::min(in.fMin, in.fMax), std::max(in.fMin, in.fMax)); break;
            case IsfInput::Long:
                if (!in.lValues.isEmpty())
                    add(path, label, *std::min_element(in.lValues.begin(), in.lValues.end()),
                        *std::max_element(in.lValues.begin(), in.lValues.end()));
                break;
            case IsfInput::Point2D: {
                const QPointF lo = in.hasPointRange ? in.pMin : QPointF(0, 0);
                const QPointF hi = in.hasPointRange ? in.pMax : QPointF(1, 1);
                add(path + "/x", label + " X", lo.x(), hi.x());
                add(path + "/y", label + " Y", lo.y(), hi.y());
                break;
            }
            case IsfInput::Color:
                for (int c = 0; c < 4; ++c) add(path + "/" + QChar("rgba"[c]), label + " " + QChar("RGBA"[c]), 0, 1);
                break;
            default: break;
            }
        }
    };
    isf(l->generator.get(), QStringLiteral("source/params/"), QStringLiteral("Source"));
    for (size_t k = 0; k < l->effects.size(); ++k)
        isf(l->effects[k].get(), QStringLiteral("effects/%1/params/").arg(k),
            QStringLiteral("FX %1 %2").arg(k + 1).arg(l->effects[k]->name()));
    return out;
}

// ---------------------------------------------------------------------------
// Project

static const char *const kWaveKeys[] = {"sine", "triangle", "saw", "square"};
static const char *const kLoopKeys[] = {"once", "loop", "pingPong"};

static int keyIndex(const char *const *keys, int n, const QString &k, int fallback)
{
    for (int i = 0; i < n; ++i)
        if (k == QLatin1String(keys[i])) return i;
    return fallback;
}

QString animLoopKey(Engine::AnimLoop l) { return QString::fromLatin1(kLoopKeys[std::clamp(int(l), 0, 2)]); }
Engine::AnimLoop animLoopFromKey(const QString &k) { return Engine::AnimLoop(keyIndex(kLoopKeys, 3, k, 1)); }

QJsonArray Engine::animationsToJson() const
{
    QJsonArray out;
    for (const Animation &a : m_animations) {
        QJsonArray tracks;
        for (const AnimTrack &t : a.tracks) {
            QJsonObject o{{"layer", QString::number(t.layer)}, {"param", t.param}};
            if (!t.enabled) o["enabled"] = false;
            if (t.oscillator) {
                o["oscillator"] = QJsonObject{{"wave", QString::fromLatin1(kWaveKeys[int(t.wave)])},
                                              {"period", t.period},
                                              {"center", t.center},
                                              {"amplitude", t.amplitude},
                                              {"phase", t.phase}};
            }
            QJsonArray keys;
            for (const AnimKey &k : t.keys) {
                QJsonArray kv{k.t, k.v};
                if (k.curve == kAnimHold) kv.append(QStringLiteral("hold"));
                else if (k.curve != 3) kv.append(k.curve);
                if (k.isCurrentValue) kv.append(true); // 4th element: current value flag
                keys.append(kv);
            }
            if (!keys.isEmpty()) o["keys"] = keys; // an oscillator keeps the curve it had, should it go back to it
            tracks.append(o);
        }
        QJsonObject o{{"id", QString::number(a.id)}, {"name", a.name}, {"duration", a.duration},
                      {"loop", animLoopKey(a.loop)}, {"tracks", tracks}};
        if (a.repeat > 0) o["repeat"] = a.repeat;
        out.append(o);
    }
    return out;
}

void Engine::animationsFromJson(const QJsonArray &arr)
{
    m_animations.clear();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Animation a;
        a.id = o.value("id").toString().toULongLong();
        a.name = o.value("name").toString();
        a.duration = std::clamp(o.value("duration").toDouble(4), kMinDuration, 36000.0);
        a.loop = animLoopFromKey(o.value("loop").toString());
        a.repeat = std::clamp(o.value("repeat").toInt(0), 0, 100000);
        for (const QJsonValue &tv : o.value("tracks").toArray()) {
            const QJsonObject to = tv.toObject();
            AnimTrack t;
            t.layer = to.value("layer").toString().toULongLong();
            t.param = to.value("param").toString();
            t.enabled = to.value("enabled").toBool(true);
            if (to.contains("oscillator")) {
                const QJsonObject w = to.value("oscillator").toObject();
                t.oscillator = true;
                t.wave = AnimWave(keyIndex(kWaveKeys, 4, w.value("wave").toString(), 0));
                t.period = std::clamp(w.value("period").toDouble(1), 0.01, 36000.0);
                t.center = w.value("center").toDouble(0.5);
                t.amplitude = w.value("amplitude").toDouble(0.5);
                t.phase = w.value("phase").toDouble(0);
            }
            for (const QJsonValue &kv : to.value("keys").toArray()) {
                const QJsonArray k = kv.toArray();
                if (k.size() < 2) continue;
                AnimKey key;
                key.t = std::clamp(k[0].toDouble(), 0.0, a.duration);
                key.v = k[1].toDouble();
                key.curve = k.size() < 3 ? 3 : k[2].toString() == "hold" ? kAnimHold : std::clamp(k[2].toInt(3), 0, 5);
                if (k.size() >= 4) key.isCurrentValue = k[3].toBool(false);
                t.keys.push_back(key);
            }
            sortKeys(t);
            a.tracks.push_back(t);
        }
        bool taken = !a.id;
        for (const Animation &x : m_animations) taken = taken || x.id == a.id;
        if (taken) a.id = 0;
        m_animations.push_back(a);
    }
    m_nextAnimationId = 1;
    for (const Animation &a : m_animations) m_nextAnimationId = std::max(m_nextAnimationId, a.id + 1);
    for (Animation &a : m_animations)
        if (!a.id) a.id = m_nextAnimationId++;
}
