// Timelines (animations): numbers of the layers drawn over time — curves and oscillators — played on their own
// clock; the sequences drive their transport.
#include "EngineInternal.h"
#include "Osc.h"

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
    if (position <= keys.front().t) return keyValue(0);
    if (position >= keys.back().t) return keyValue(keys.size() - 1);
    for (size_t k = 0; k + 1 < keys.size(); ++k) {
        const AnimKey &a = keys[k], &b = keys[k + 1];
        if (position >= b.t) continue;
        const double va = keyValue(k), vb = keyValue(k + 1);
        if (a.curve == kAnimHold || b.t - a.t <= 1e-9) return va;
        return va + (vb - va) * easeCurve((position - a.t) / (b.t - a.t), a.curve);
    }
    return keyValue(keys.size() - 1);
}

double Engine::AnimTrack::keyValue(size_t k) const
{
    if (k == 0 && !keys.empty() && keys[0].isCurrentValue && std::isfinite(captured)) return captured;
    return k < keys.size() ? keys[k].v : std::numeric_limits<double>::quiet_NaN();
}

static double passDuration(double d) { return std::max(kMinDuration, d); }

double Engine::Animation::length() const
{
    const double d = passDuration(duration);
    if (loop == AnimLoop::Once) return d;
    return repeat > 0 ? d * repeat : std::numeric_limits<double>::infinity();
}

// The pass at clock c (0, 1…) and the time within it
static void passAt(const Engine::Animation &a, double c, double *pass, double *within)
{
    const double d = passDuration(a.duration);
    c = std::clamp(c, 0.0, a.length());
    if (a.loop == Engine::AnimLoop::Once) {
        *pass = 0;
        *within = std::min(c, d);
        return;
    }
    *pass = std::floor(c / d);
    *within = c - *pass * d;
    if (std::isfinite(a.length()) && c >= a.length()) { // the end of the last pass
        *pass = std::max(0, a.repeat - 1);
        *within = d;
    }
}

bool Engine::Animation::backwardsAt(double c) const
{
    double pass = 0, within = 0;
    passAt(*this, c, &pass, &within);
    if (loop == AnimLoop::PingPong) return std::fmod(pass + (reversed ? 1 : 0), 2.0) == 1.0;
    return reversed && pass == 0;
}

double Engine::Animation::position(double c) const
{
    double pass = 0, within = 0;
    passAt(*this, c, &pass, &within);
    return backwardsAt(c) ? passDuration(duration) - within : within;
}

double Engine::Animation::clockAt(double pos) const
{
    const double d = passDuration(duration);
    pos = std::clamp(pos, 0.0, d);
    double pass = 0, within = 0;
    passAt(*this, clock, &pass, &within);
    return pass * d + (backwardsAt(clock) ? d - pos : pos);
}

void Engine::Animation::setLoop(AnimLoop l, int n)
{
    n = std::max(0, n);
    if (l == loop && n == repeat) return;
    if (state == AnimState::Stopped) {
        loop = l;
        repeat = n;
        clock = 0;
        reversed = false;
        return;
    }
    const double pos = position(), d = passDuration(duration);
    const bool back = backwardsAt(clock);
    loop = l;
    repeat = n;
    reversed = back;
    clock = back ? d - pos : pos; // within the first pass of the new mode
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
        const Animation old = x;
        x = a;
        x.id = old.id;
        // Where it is stays: its clock, its way, the values captured when it started
        x.state = old.state;
        x.clock = old.clock;
        x.reversed = old.reversed;
        x.loop = old.loop;
        x.repeat = old.repeat;
        x.setLoop(a.loop, a.repeat); // a new loop mode: from where it is, without a jump
        x.clock = std::min(x.clock, x.length());
        for (AnimTrack &t : x.tracks) {
            sortKeys(t);
            t.captured = std::numeric_limits<double>::quiet_NaN();
            for (const AnimTrack &o : old.tracks)
                if (o.layer == t.layer && o.param == t.param) t.captured = o.captured;
        }
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
    if (action == AnimAction::LoopMode || action == AnimAction::Speed) emit animationsChanged(); // its definition (saved with it)
}

void Engine::controlLocked(Animation *a, AnimAction action, double time, AnimLoop loop, int repeat)
{
    // The values the "current value" keys start from: read as the timeline starts
    auto capture = [this, a] {
        for (AnimTrack &t : a->tracks) {
            double v = 0;
            const bool cur = !t.oscillator && !t.keys.empty() && t.keys.front().isCurrentValue;
            t.captured = cur && animParamValue(t.layer, t.param, &v) ? v : std::numeric_limits<double>::quiet_NaN();
        }
    };
    switch (action) {
    case AnimAction::Play:
        if (a->state == AnimState::Stopped) {
            a->clock = 0;
            a->reversed = false;
            capture();
        }
        a->state = AnimState::Playing;
        applyAnimation(*a); // its first values now, not a frame later
        break;
    case AnimAction::Pause:
        if (a->state == AnimState::Playing) a->state = AnimState::Paused;
        break;
    case AnimAction::Stop:
        a->state = AnimState::Stopped;
        a->clock = 0;
        a->reversed = false;
        break;
    case AnimAction::Rewind:
        a->clock = 0;
        a->reversed = false;
        capture();
        applyAnimation(*a);
        break;
    case AnimAction::Seek:
        if (a->state == AnimState::Stopped) {
            a->state = AnimState::Paused; // stays where it was sought
            a->reversed = false;
            capture();
        }
        a->clock = std::clamp(time, 0.0, std::min(a->length(), 1e9));
        applyAnimation(*a);
        break;
    case AnimAction::Speed:
        a->speed = std::clamp(time, 0.0, 10.0); // the clock goes on from where it is: no jump
        break;
    case AnimAction::LoopMode:
        a->setLoop(loop, repeat); // from where it is: Once ends the pass it is in, no jump to the end
        break;
    }
}

void Engine::applyAnimation(Animation &a)
{
    const double pos = a.position(), played = std::min(a.clock, a.length());
    for (const AnimTrack &t : a.tracks) {
        if (!t.enabled || t.param.isEmpty()) continue;
        const double v = t.valueAt(pos, played);
        if (std::isfinite(v)) setAnimParam(t.layer, t.param, v);
    }
}

void Engine::stepAnimations(double dt)
{
    dt = std::max(0.0, dt);
    for (Animation &a : m_animations) {
        if (a.state != AnimState::Playing) continue;
        a.clock += dt * a.speed;
        applyAnimation(a);
        if (a.clock >= a.length()) { // over: its last values stay
            a.state = AnimState::Stopped;
            a.clock = 0;
            a.reversed = false;
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
    if (path == "opacity") return num(l.opacity, 0, 1);
    if (a == "roi" && p.size() == 2) {
        const int side = QStringList{"left", "top", "right", "bottom"}.indexOf(p[1]);
        if (side < 0) return false;
        double v[4] = {l.roi.left(), l.roi.top(), l.roi.right(), l.roi.bottom()};
        if (get) *get = v[side];
        if (set) {
            v[side] = std::clamp(*set, 0.0, 1.0);
            const double minSize = 0.002;
            if (side == 0) v[0] = std::min(v[0], v[2] - minSize);
            if (side == 2) v[2] = std::max(v[2], v[0] + minSize);
            if (side == 1) v[1] = std::min(v[1], v[3] - minSize);
            if (side == 3) v[3] = std::max(v[3], v[1] + minSize);
            l.roi = QRectF(QPointF(v[0], v[1]), QPointF(v[2], v[3])) & Layer::fullRoi();
        }
        return true;
    }
    if (a == "source") {
        if (path == "source/volume") return num(l.volume, 0, 2);
        if (path == "source/speed") {
            if (!l.hasTransport() && l.generator) return num(l.generator->speed, 0, 10); // the shader's time
            return num(l.speed, -16, 16);
        }
        if (p.value(1) == "text" && l.isText()) {
            TextSource &t = l.text;
            const QString sub = p.mid(2).join('/');
            auto io = [&](double cur, double lo, double hi, auto store) {
                if (get) *get = cur;
                if (set) store(std::clamp(*set, lo, hi));
                return true;
            };
            if (sub == "size") return io(t.size, 1, 1000, [&](double x) { t.size = int(std::lround(x)); });
            if (sub == "line_height") return io(t.lineHeight, 0.1, 10, [&](double x) { t.lineHeight = float(x); });
            if (sub == "letter_spacing") return io(t.letterSpacing, -200, 500, [&](double x) { t.letterSpacing = float(x); });
            if (sub == "outline") return io(t.outline, 0, 200, [&](double x) { t.outline = float(x); });
            if (sub == "shadow/x") return io(t.shadowX, -2000, 2000, [&](double x) { t.shadowX = float(x); });
            if (sub == "shadow/y") return io(t.shadowY, -2000, 2000, [&](double x) { t.shadowY = float(x); });
            return false;
        }
    }
    if (a == "color") {
        ColorAdjust &c = l.color;
        if (path == "color/temp") return num(c.temp, -ColorAdjust::kTempRange, ColorAdjust::kTempRange);
        if (path == "color/tint") return num(c.tint, -ColorAdjust::kTintRange, ColorAdjust::kTintRange);
        if ((p.value(1) == "add" || p.value(1) == "remove") && p.size() == 3) {
            const int k = QStringLiteral("rgb").indexOf(p[2]);
            if (k < 0 || p[2].size() != 1) return false;
            return num(p[1] == "add" ? c.add[k] : c.remove[k], 0, 1);
        }
        return false;
    }
    if (a == "spatial") {
        Mapping &m = l.mapping;
        if (path == "spatial/rotation") {
            const double now = m.angle(comp);
            if (get) *get = now;
            if (set) m.rotate(wrapDegrees(*set - now), comp);
            return true;
        }
        if (l.isViewport && ((p.size() == 2 && (p[1] == "width" || p[1] == "height")) ||
                             (p.size() == 3 && p[1] == "position" && (p[2] == "x" || p[2] == "y")))) {
            Mapping::Rect r = m.rect(comp); // a rectangle, upright or turned: pixels
            double *v = p[1] == "width" ? &r.w : p[1] == "height" ? &r.h : p[2] == "x" ? &r.center.rx() : &r.center.ry();
            if (get) *get = *v;
            if (set) {
                *v = p[1] == "position" ? std::clamp(*set, -4.0 * (p[2] == "x" ? comp.width() : comp.height()),
                                                     5.0 * (p[2] == "x" ? comp.width() : comp.height()))
                                        : std::clamp(*set, 1.0, 100000.0);
                m.setRect(r, comp);
            }
            return true;
        }
        if (p.size() == 3 && p[1] == "pivot" && (p[2] == "x" || p[2] == "y")) { // center of the rotation, in pixels
            const bool x = p[2] == "x";
            QPointF pt = m.pivotPoint();
            const double size = x ? comp.width() : comp.height();
            if (get) *get = (x ? pt.x() : pt.y()) * size;
            if (set) {
                (x ? pt.rx() : pt.ry()) = std::clamp(*set, -4.0 * size, 5.0 * size) / size;
                m.setPivotPoint(pt);
            }
            return true;
        }
        if (p.size() == 3 && (p[1] == "position" || (p[1] == "scale" && !l.isViewport)) && (p[2] == "x" || p[2] == "y")) {
            const bool x = p[2] == "x";
            QRectF b = m.bounds();
            const double size = x ? comp.width() : comp.height();
            // position: the centre, in composition pixels; scale: % of the composition
            const double now = p[1] == "position" ? (x ? b.center().x() : b.center().y()) * size : (x ? b.width() : b.height()) * 100.0;
            if (get) *get = now;
            if (set) {
                if (p[1] == "position") {
                    QPointF c = b.center();
                    const double v = std::clamp(*set, -4.0 * size, 5.0 * size) / size;
                    (x ? c.rx() : c.ry()) = v;
                    b.moveCenter(c);
                } else {
                    const QPointF c = b.center();
                    const double v = std::clamp(*set, 0.1, 2000.0) / 100.0;
                    b.setSize(QSizeF(x ? v : b.width(), x ? b.height() : v));
                    b.moveCenter(c);
                }
                m.setBounds(b);
            }
            return true;
        }
        return false;
    }
    if (a == "fx" && p.size() == 3 && p[2] == "speed") { // an effect's time
        QStringList names;
        for (const auto &x : l.effects) names << x->name();
        const int k = osc::uniqueSegments(names).indexOf(p[1]);
        if (k < 0) return false;
        return num(l.effects[size_t(k)]->speed, 0, 10);
    }
    // ISF: source/param/<name>[/<x|y|r|g|b|a>], effect/<fx>/param/<name>[/…] (fx: its segment, as in the OSC address)
    IsfInstance *inst = nullptr;
    int at = 0;
    if (a == "source" && p.size() >= 3 && p[1] == "param") {
        inst = l.generator.get();
        at = 2;
    } else if (a == "fx" && p.size() >= 4 && p[2] == "param") {
        QStringList names;
        for (const auto &x : l.effects) names << x->name();
        const int k = osc::uniqueSegments(names).indexOf(p[1]);
        if (k >= 0) inst = l.effects[size_t(k)].get();
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
        if (path == "opacity") {
            m_compositionOpacityTarget = std::clamp(v, 0.0, 1.0);
            m_compositionOpacitySpeed = 0;
            return true;
        }
        if (path == "volume") {
            m_audio->setVolume(float(std::clamp(v, 0.0, 2.0)));
            return true;
        }
        if (path == "speed") {
            setCompositionSpeed(v);
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
        if (path == "opacity") *value = m_compositionOpacityTarget;
        else if (path == "volume") *value = m_audio->volume();
        else if (path == "speed") *value = m_compositionSpeed.load();
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
        add("opacity", "Opacity", 0, 1);
        add("volume", "Volume", 0, 2);
        add("speed", "Speed", 0, 10);
        return out;
    }
    const Layer *l = nullptr;
    for (auto &x : m_layers)
        if (x->id == layer) l = x.get();
    if (!l) return out;
    // Labels: "<Category> › <Parameter>", the categories being the branches of the OSC address
    add("opacity", "Opacity", 0, 1);
    add("spatial/rotation", "Spatial › Rotation (°)", -180, 180);
    add("spatial/position/x", "Spatial › Position X", -4.0 * m_compSize.width(), 5.0 * m_compSize.width());
    add("spatial/position/y", "Spatial › Position Y", -4.0 * m_compSize.height(), 5.0 * m_compSize.height());
    add("spatial/pivot/x", "Spatial › Pivot X", -4.0 * m_compSize.width(), 5.0 * m_compSize.width());
    add("spatial/pivot/y", "Spatial › Pivot Y", -4.0 * m_compSize.height(), 5.0 * m_compSize.height());
    if (l->isViewport) { // an upright rectangle of the composition: its size in pixels
        add("spatial/width", "Spatial › Width", 1, 100000);
        add("spatial/height", "Spatial › Height", 1, 100000);
    } else {
        add("spatial/scale/x", "Spatial › Scale X (%)", 0.1, 2000);
        add("spatial/scale/y", "Spatial › Scale Y (%)", 0.1, 2000);
    }
    if (l->hasTransport()) {
        add("source/volume", "Source › Volume", 0, 2);
        add("source/speed", "Source › Speed", 0, 4);
    } else if (l->generator && l->generator->isValid()) {
        add("source/speed", "Source › Speed", 0, 10);
    }
    if (!l->isGroup) {
        add("roi/left", "ROI › Left", 0, 1);
        add("roi/top", "ROI › Top", 0, 1);
        add("roi/right", "ROI › Right", 0, 1);
        add("roi/bottom", "ROI › Bottom", 0, 1);
    }
    add("color/temp", "Color › Temperature", -ColorAdjust::kTempRange, ColorAdjust::kTempRange);
    add("color/tint", "Color › Tint", -ColorAdjust::kTintRange, ColorAdjust::kTintRange);
    for (const char *k : {"add", "remove"})
        for (int c = 0; c < 3; ++c)
            add(QStringLiteral("color/%1/%2").arg(QLatin1String(k)).arg(QChar("rgb"[c])),
                QStringLiteral("Color › %1 %2").arg(QLatin1String(k[0] == 'a' ? "Add" : "Remove")).arg(QChar("RGB"[c])), 0, 1);
    if (l->isText()) {
        add("source/text/size", "Source › Text Size", 1, 400);
        add("source/text/line_height", "Source › Text Line Height", 0.5, 3);
        add("source/text/letter_spacing", "Source › Text Letter Spacing", -20, 100);
        add("source/text/outline", "Source › Text Outline", 0, 40);
        add("source/text/shadow/x", "Source › Text Shadow X", -100, 100);
        add("source/text/shadow/y", "Source › Text Shadow Y", -100, 100);
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
    isf(l->generator.get(), QStringLiteral("source/param/"), QStringLiteral("Source"));
    QStringList fxNames;
    for (const auto &x : l->effects) fxNames << x->name();
    const QStringList fxSegs = osc::uniqueSegments(fxNames);
    for (size_t k = 0; k < l->effects.size(); ++k) {
        add(QStringLiteral("fx/%1/speed").arg(fxSegs[int(k)]), QStringLiteral("FX › %1 › Speed").arg(l->effects[k]->name()), 0, 10);
        isf(l->effects[k].get(), QStringLiteral("fx/%1/param/").arg(fxSegs[int(k)]), QStringLiteral("FX › %1").arg(l->effects[k]->name()));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Project

static const char *const kWaveKeys[] = {"sine", "triangle", "saw", "square"};
static const char *const kLoopKeys[] = {"once", "loop", "pingpong"};

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
            if (!t.enabled) o["enable"] = false;
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
                else if (k.curve != 3 || k.isCurrentValue) kv.append(k.curve);
                if (k.isCurrentValue) kv.append(QStringLiteral("current")); // [t, v, curve, "current"]
                keys.append(kv);
            }
            if (!keys.isEmpty()) o["keys"] = keys; // an oscillator keeps the curve it had, should it go back to it
            tracks.append(o);
        }
        QJsonObject o{{"id", QString::number(a.id)}, {"name", a.name}, {"duration", a.duration},
                      {"loop", animLoopKey(a.loop)}, {"tracks", tracks}};
        if (a.repeat > 0) o["repeat"] = a.repeat;
        if (a.speed != 1.0) o["speed"] = a.speed;
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
        a.speed = std::clamp(o.value("speed").toDouble(1.0), 0.0, 10.0);
        for (const QJsonValue &tv : o.value("tracks").toArray()) {
            const QJsonObject to = tv.toObject();
            AnimTrack t;
            t.layer = to.value("layer").toString().toULongLong();
            t.param = to.value("param").toString();
            t.enabled = to.value("enable").toBool(true);
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
                key.isCurrentValue = t.keys.empty() && k.size() >= 4 && k[3].toString() == "current"; // the first key only
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
