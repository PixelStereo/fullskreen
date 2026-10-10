// Timelines (animations): numbers of the layers drawn over time — curves and oscillators — played on their own
// clock; the sequences drive their transport.
#include "EngineInternal.h"
#include "Osc.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <cmath>
#include <limits>

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kMinDuration = 0.05;

// ---------------------------------------------------------------------------
// Values

// A random number in -1..1 for (seed, n): the same every time
static double hashUnit(quint32 seed, long long n)
{
    quint64 x = (quint64(seed) << 32) ^ quint64(n) ^ 0x9E3779B97F4A7C15ull;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return double(x >> 11) / double(1ull << 53) * 2.0 - 1.0;
}

// The value of a cubic Bézier (one coordinate) at s in 0..1
static double cubic(double a, double b, double c, double d, double s)
{
    const double u = 1 - s;
    return u * u * u * a + 3 * u * u * s * b + 3 * u * s * s * c + s * s * s * d;
}

void AnimTrack::bezierHandles(size_t k, double *t1, double *v1, double *t2, double *v2) const
{
    const AnimKey &a = keys[k], &b = keys[k + 1];
    const double va = keyValue(k), vb = keyValue(k + 1), span = std::max(0.0, b.t - a.t);
    // Automatic: flat, a third of the way; a handle never reaches beyond the other key (the curve stays a function of time)
    *t1 = a.t + (a.hasOut() ? std::clamp(a.outDt, 0.0, span) : span / 3);
    *v1 = va + (a.hasOut() ? a.outDv : 0.0);
    *t2 = b.t + (b.hasIn() ? std::clamp(b.inDt, -span, 0.0) : -span / 3);
    *v2 = vb + (b.hasIn() ? b.inDv : 0.0);
}

double AnimTrack::valueAt(double position, double played) const
{
    if (oscillator) {
        const double per = std::max(0.01, period);
        const double total = played / per + phase; // periods since the start
        double p = total - std::floor(total);      // 0..1 within the period
        double w = 0;
        switch (wave) {
        case AnimWave::Sine: w = std::sin(2 * kPi * p); break;
        case AnimWave::Triangle: w = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4; break; // starts at 0, rises
        case AnimWave::Saw: w = 2 * p - 1; break;
        case AnimWave::Square: w = p < 0.5 ? 1 : -1; break;
        case AnimWave::Random: w = hashUnit(seed, (long long)std::floor(total)); break;
        case AnimWave::SmoothRandom: {
            const long long n = (long long)std::floor(total);
            const double e = (1 - std::cos(kPi * p)) / 2; // eased from one value to the next
            w = hashUnit(seed, n) + (hashUnit(seed, n + 1) - hashUnit(seed, n)) * e;
            break;
        }
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
        if (a.curve == kAnimBezier) {
            double t1, v1, t2, v2;
            bezierHandles(k, &t1, &v1, &t2, &v2);
            // The curve's time grows with s (the handles stay within the segment): s found by halving
            double lo = 0, hi = 1;
            for (int i = 0; i < 40; ++i) {
                const double mid = (lo + hi) / 2;
                (cubic(a.t, t1, t2, b.t, mid) < position ? lo : hi) = mid;
            }
            return cubic(va, v1, v2, vb, (lo + hi) / 2);
        }
        return va + (vb - va) * easeCurve((position - a.t) / (b.t - a.t), a.curve);
    }
    return keyValue(keys.size() - 1);
}

double AnimTrack::keyValue(size_t k) const
{
    if (k == 0 && !keys.empty() && keys[0].isCurrentValue && std::isfinite(captured)) return captured;
    return k < keys.size() ? keys[k].v : std::numeric_limits<double>::quiet_NaN();
}

static double passDuration(double d) { return std::max(kMinDuration, d); }

double Animation::length() const
{
    const double d = passDuration(duration);
    if (loop == AnimLoop::Once) return d;
    return repeat > 0 ? d * repeat : std::numeric_limits<double>::infinity();
}

// The pass at clock c (0, 1…) and the time within it
static void passAt(const Animation &a, double c, double *pass, double *within)
{
    const double d = passDuration(a.duration);
    c = std::clamp(c, 0.0, a.length());
    if (a.loop == AnimLoop::Once) {
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

bool Animation::backwardsAt(double c) const
{
    double pass = 0, within = 0;
    passAt(*this, c, &pass, &within);
    if (loop == AnimLoop::PingPong) return std::fmod(pass + (reversed ? 1 : 0), 2.0) == 1.0;
    return reversed && pass == 0;
}

double Animation::position(double c) const
{
    double pass = 0, within = 0;
    passAt(*this, c, &pass, &within);
    return backwardsAt(c) ? passDuration(duration) - within : within;
}

double Animation::clockAt(double pos) const
{
    const double d = passDuration(duration);
    pos = std::clamp(pos, 0.0, d);
    double pass = 0, within = 0;
    passAt(*this, clock, &pass, &within);
    return pass * d + (backwardsAt(clock) ? d - pos : pos);
}

void Animation::setLoop(AnimLoop l, int n)
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

bool Animation::advance(double dt)
{
    clock += std::max(0.0, dt) * speed;
    return clock < length();
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

static void sortKeys(AnimTrack &t)
{
    std::stable_sort(t.keys.begin(), t.keys.end(), [](const AnimKey &a, const AnimKey &b) { return a.t < b.t; });
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

void Engine::controlLocked(Animation *a, AnimAction action, double time, AnimLoop loop, int repeat, Layer *owner)
{
    auto apply = [this, a, owner] {
        if (owner) applyLayerAnim(*owner, *a);
        else applyAnimation(*a);
    };
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
        apply(); // its first values now, not a frame later
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
        apply();
        break;
    case AnimAction::Seek:
        if (a->state == AnimState::Stopped) {
            a->state = AnimState::Paused; // stays where it was sought
            a->reversed = false;
            capture();
        }
        a->clock = std::clamp(time, 0.0, std::min(a->length(), 1e9));
        apply();
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
        if (std::isfinite(v) && setAnimParam(t.layer, t.param, v) && t.layer) releaseFromFades(t.layer, t.param);
    }
}

void Engine::stepAnimations(double dt)
{
    dt = std::max(0.0, dt);
    // The layers' own numbers first: a timeline playing the same number wins
    for (auto &lp : m_layers) {
        Layer &l = *lp;
        for (Animation &a : l.anims) {
            if (a.state != AnimState::Playing || a.tracks.empty() || !a.tracks.front().enabled) continue;
            const bool going = a.advance(dt);
            applyLayerAnim(l, a);
            if (!going) { // over: its last values stay
                a.state = AnimState::Stopped;
                a.clock = 0;
                a.reversed = false;
            }
        }
    }
    for (Animation &a : m_animations) {
        if (a.state != AnimState::Playing) continue;
        const bool going = a.advance(dt);
        applyAnimation(a);
        if (!going) { // over: its last values stay
            a.state = AnimState::Stopped;
            a.clock = 0;
            a.reversed = false;
        }
    }
}

// ---------------------------------------------------------------------------
// The numbers a timeline drives

const std::vector<Parameter *> &Engine::compositionParameters()
{
    if (!m_compParamList.empty()) return m_compParamList;
    auto add = [this](const QString &path, const QString &label) -> Parameter & {
        m_compParams.push_back(std::make_unique<Parameter>(path, label, Parameter::Type::Float));
        return *m_compParams.back();
    };
    add("opacity", "Opacity").range(0, 1).byDefault(1.0).bind([this] { return QVariant(m_compositionOpacityTarget); },
                                                               [this](const QVariant &v) {
                                                                   m_compositionOpacityTarget = v.toDouble();
                                                                   m_compositionOpacitySpeed = 0;
                                                               });
    add("volume", "Volume").range(0, 2).byDefault(1.0).bind([this] { return QVariant(double(m_audio->volume())); },
                                                             [this](const QVariant &v) { m_audio->setVolume(float(v.toDouble())); });
    add("speed", "Speed").range(0, 10).byDefault(1.0).bind([this] { return QVariant(m_compositionSpeed.load()); },
                                                           [this](const QVariant &v) { setCompositionSpeed(v.toDouble()); });
    for (auto &p : m_compParams) m_compParamList.push_back(p.get());
    return m_compParamList;
}

Parameter *Engine::findParameter(quint64 layer, const QString &path)
{
    if (!layer) {
        for (Parameter *p : compositionParameters())
            if (p->path() == path) return p;
        return nullptr;
    }
    for (auto &l : m_layers)
        if (l->id == layer) return l->parameter(path);
    return nullptr;
}

bool Engine::setAnimParam(quint64 layer, const QString &path, double v)
{
    if (layer)
        for (auto &l : m_layers)
            if (l->id == layer && l->locked) return false; // a timeline leaves a locked layer alone
    Parameter *p = findParameter(layer, path);
    return p && p->isNumber() && p->setNumber(v);
}

bool Engine::animParamValue(quint64 layer, const QString &path, double *value) const
{
    Lock lk(&m_mutex);
    const Parameter *p = const_cast<Engine *>(this)->findParameter(layer, path);
    if (!p || !p->isNumber()) return false;
    *value = p->number();
    return true;
}

std::vector<ParamInfo> Engine::parameters(quint64 layer) const
{
    Lock lk(&m_mutex);
    std::vector<ParamInfo> out;
    auto *self = const_cast<Engine *>(this); // the lists are made when they are asked for
    const std::vector<Parameter *> *list = nullptr;
    if (!layer) list = &self->compositionParameters();
    for (auto &l : m_layers)
        if (l->id == layer) list = &l->parameters();
    if (list)
        for (const Parameter *p : *list) out.push_back(p->info());
    return out;
}

std::vector<Engine::AnimParam> Engine::animatableParams(quint64 layer) const
{
    std::vector<AnimParam> out;
    for (const ParamInfo &i : parameters(layer))
        if (i.animatable) out.push_back(i);
    return out;
}

bool Engine::parameterInfo(quint64 layer, const QString &path, ParamInfo *info) const
{
    Lock lk(&m_mutex);
    const Parameter *p = const_cast<Engine *>(this)->findParameter(layer, path);
    if (!p) return false;
    if (info) *info = p->info();
    return true;
}

// ---------------------------------------------------------------------------
// The animations of a layer's numbers

void Engine::applyLayerAnim(Layer &l, Animation &a)
{
    if (a.tracks.empty()) return;
    const AnimTrack &t = a.tracks.front();
    if (!t.enabled || t.param.isEmpty()) return;
    const double v = t.valueAt(a.position(), std::min(a.clock, a.length()));
    Parameter *p = l.parameter(t.param);
    if (std::isfinite(v) && p && p->setNumber(v)) releaseFromFades(l.id, t.param);
}

void Engine::startLayerAnim(Animation &a)
{
    a.state = AnimState::Stopped; // Play from the start, its "current value" keys read now
    Layer *owner = nullptr;
    for (auto &lp : m_layers)
        for (Animation &x : lp->anims)
            if (&x == &a) owner = lp.get();
    controlLocked(&a, AnimAction::Play, 0, a.loop, a.repeat, owner);
}

static Animation *findLayerAnim(Layer &l, const QString &param)
{
    for (Animation &a : l.anims)
        if (!a.tracks.empty() && a.tracks.front().param == param) return &a;
    return nullptr;
}

std::vector<Animation> Engine::layerAnims(quint64 layer) const
{
    Lock lk(&m_mutex);
    for (auto &l : m_layers)
        if (l->id == layer) return l->anims;
    return {};
}

bool Engine::layerAnim(quint64 layer, const QString &param, Animation *a) const
{
    Lock lk(&m_mutex);
    for (auto &l : m_layers)
        if (l->id == layer)
            if (const Animation *x = findLayerAnim(*l, param)) {
                if (a) *a = *x;
                return true;
            }
    return false;
}

void Engine::assignLayerAnims(Layer &l, const std::vector<Animation> &anims)
{
    std::vector<Animation> old = std::move(l.anims);
    l.anims.clear();
    for (const Animation &in : anims) {
        if (in.tracks.empty() || in.tracks.front().param.isEmpty() || findLayerAnim(l, in.tracks.front().param))
            continue; // one animation per number
        Animation a = in;
        a.tracks.resize(1);
        AnimTrack &t = a.tracks.front();
        t.layer = l.id;
        sortKeys(t);
        a.state = AnimState::Stopped;
        a.clock = 0;
        a.reversed = false;
        const Animation *was = nullptr;
        for (const Animation &o : old)
            if (!o.tracks.empty() && o.tracks.front().param == t.param) was = &o;
        if (was) { // how its card is shown is not part of an edit
            a.pinned = was->pinned;
            a.folded = was->folded;
        }
        if (was && was->tracks.front().enabled && t.enabled) { // it goes on from where it is
            a.state = was->state;
            a.clock = was->clock;
            a.reversed = was->reversed;
            a.loop = was->loop;
            a.repeat = was->repeat;
            a.setLoop(in.loop, in.repeat);
            a.clock = std::min(a.clock, a.length());
            t.captured = was->tracks.front().captured;
        }
        l.anims.push_back(a);
    }
    // New, or turned on: from the start
    for (Animation &a : l.anims) {
        const QString &param = a.tracks.front().param;
        bool wasOn = false;
        for (const Animation &o : old)
            if (!o.tracks.empty() && o.tracks.front().param == param) wasOn = o.tracks.front().enabled;
        if (a.tracks.front().enabled && !wasOn) controlLocked(&a, AnimAction::Play, 0, a.loop, a.repeat, &l);
    }
}

void Engine::setLayerAnims(quint64 layer, const std::vector<Animation> &anims)
{
    {
        Lock lk(&m_mutex);
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == layer) l = x.get();
        if (!l) return;
        assignLayerAnims(*l, anims);
    }
    emit layerAnimsChanged(layer);
}

void Engine::setLayerAnim(quint64 layer, const QString &param, const Animation *a, int at)
{
    {
        Lock lk(&m_mutex);
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == layer) l = x.get();
        if (!l) return;
        std::vector<Animation> list = l->anims; // the others as they are
        auto it = std::find_if(list.begin(), list.end(), [&](const Animation &x) { return x.tracks.front().param == param; });
        if (a && it != list.end()) {
            *it = *a;
        } else if (a) {
            list.insert(at >= 0 && at <= int(list.size()) ? list.begin() + at : list.end(), *a);
        } else if (it != list.end()) {
            list.erase(it);
        } else {
            return;
        }
        assignLayerAnims(*l, list);
    }
    emit layerAnimsChanged(layer);
}

void Engine::setLayerAnimOn(quint64 layer, const QString &param, bool on)
{
    {
        Lock lk(&m_mutex); // read and written under the same lock: an edit meanwhile is not lost
        Layer *l = nullptr;
        for (auto &x : m_layers)
            if (x->id == layer) l = x.get();
        Animation *a = l ? findLayerAnim(*l, param) : nullptr;
        if (!a || a->tracks.front().enabled == on) return;
        std::vector<Animation> list = l->anims;
        for (Animation &x : list)
            if (x.tracks.front().param == param) x.tracks.front().enabled = on;
        assignLayerAnims(*l, list);
    }
    emit layerAnimsChanged(layer);
}

int Engine::layerAnimIndex(quint64 layer, const QString &param) const
{
    Lock lk(&m_mutex);
    for (auto &l : m_layers)
        if (l->id == layer)
            for (size_t i = 0; i < l->anims.size(); ++i)
                if (l->anims[i].tracks.front().param == param) return int(i);
    return -1;
}

void Engine::setLayerAnimView(quint64 layer, const QString &param, bool pinned, bool folded)
{
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers)
            if (l->id == layer)
                if (Animation *a = findLayerAnim(*l, param)) {
                    a->pinned = pinned;
                    a->folded = folded;
                }
    }
    emit layerAnimsChanged(layer);
}

void Engine::controlLayerAnim(quint64 layer, const QString &param, AnimAction action, double time, AnimLoop loop, int repeat)
{
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers)
            if (l->id == layer)
                if (Animation *a = findLayerAnim(*l, param)) controlLocked(a, action, time, loop, repeat, l.get());
    }
    if (action == AnimAction::LoopMode || action == AnimAction::Speed) emit layerAnimsChanged(layer); // its definition
}

void Engine::waveAround(quint64 layer, const QString &param, double *center, double *amplitude) const
{
    double lo = 0, hi = 1, v = 0;
    for (const AnimParam &p : animatableParams(layer))
        if (p.path == param) lo = p.min, hi = p.max;
    if (!animParamValue(layer, param, &v)) v = (lo + hi) / 2;
    QSize comp;
    {
        Lock lk(&m_mutex);
        comp = m_compSize;
    }
    const bool horizontal = param.endsWith("/x") || param.endsWith("/width");
    *center = v;
    ParamInfo info;
    parameterInfo(layer, param, &info);
    if (info.ramp == ParamInfo::Ramp::Angle) {
        *center = 0; // a saw turns round and round, a sine swings
        *amplitude = 180;
    } else if (param.startsWith("position/") || param.startsWith("pivot/") || param == "width" || param == "height") {
        *amplitude = 0.1 * (horizontal ? comp.width() : comp.height()); // pixels: a tenth of the composition
    } else if (param.startsWith("scale/")) {
        *amplitude = 25;
    } else {
        *amplitude = std::min(v - lo, hi - v); // as far as it can go both ways…
        if (*amplitude < 0.1 * (hi - lo) / 2) { // …or, at a bound, the whole range
            *center = (lo + hi) / 2;
            *amplitude = (hi - lo) / 2;
        }
    }
}

Animation Engine::makeLayerAnim(quint64 layer, const QString &param, int wave) const
{
    double v = 0, center = 0, amp = 0;
    waveAround(layer, param, &center, &amp);
    if (!animParamValue(layer, param, &v)) v = center;
    Animation a;
    a.duration = 4;
    a.loop = AnimLoop::Loop;
    AnimTrack t;
    t.layer = layer;
    t.param = param;
    t.seed = QRandomGenerator::global()->generate() | 1u;
    if (wave >= 0) {
        t.oscillator = true;
        t.wave = AnimWave(std::clamp(wave, 0, kAnimWaveCount - 1));
        t.period = 2;
        t.center = center;
        t.amplitude = amp;
    } else { // from where it is, out and back
        const double far = std::abs(center + amp - v) >= std::abs(center - amp - v) ? center + amp : center - amp;
        t.keys = {AnimKey{0, v, 3}, AnimKey{a.duration / 2, far, 3}, AnimKey{a.duration, v, 3}};
    }
    a.tracks.push_back(t);
    return a;
}

// ---------------------------------------------------------------------------
// Project

static const char *const kWaveKeys[] = {"sine", "triangle", "saw", "square", "random", "smooth_random"};
static const char *const kLoopKeys[] = {"once", "loop", "pingpong"};

static int keyIndex(const char *const *keys, int n, const QString &k, int fallback)
{
    for (int i = 0; i < n; ++i)
        if (k == QLatin1String(keys[i])) return i;
    return fallback;
}

QStringList animWaveKeys()
{
    QStringList k;
    for (const char *x : kWaveKeys) k << QString::fromLatin1(x);
    return k;
}

QString animLoopKey(Engine::AnimLoop l) { return QString::fromLatin1(kLoopKeys[std::clamp(int(l), 0, 2)]); }
Engine::AnimLoop animLoopFromKey(const QString &k) { return Engine::AnimLoop(keyIndex(kLoopKeys, 3, k, 1)); }

// What drives a number (a timeline's track, or a layer's animation), into a JSON object: its keys
// [t, v, curve, "current", {"in": [dt, dv], "out": [dt, dv]}] (all but t and v when they matter) and its oscillator
static void trackToJson(const AnimTrack &t, QJsonObject &o)
{
    if (!t.enabled) o["enable"] = false;
    if (t.oscillator) {
        o["oscillator"] = QJsonObject{{"wave", QString::fromLatin1(kWaveKeys[std::clamp(int(t.wave), 0, kAnimWaveCount - 1)])},
                                      {"period", t.period},
                                      {"center", t.center},
                                      {"amplitude", t.amplitude},
                                      {"phase", t.phase},
                                      {"seed", double(t.seed)}};
    }
    QJsonArray keys;
    for (const AnimKey &k : t.keys) {
        QJsonArray kv{k.t, k.v};
        QJsonObject handles;
        if (k.hasIn()) handles["in"] = QJsonArray{k.inDt, k.inDv};
        if (k.hasOut()) handles["out"] = QJsonArray{k.outDt, k.outDv};
        const bool more = k.isCurrentValue || !handles.isEmpty();
        if (k.curve == kAnimHold) kv.append(QStringLiteral("hold"));
        else if (k.curve == kAnimBezier) kv.append(QStringLiteral("bezier"));
        else if (k.curve != 3 || more) kv.append(k.curve);
        if (k.isCurrentValue) kv.append(QStringLiteral("current"));
        if (!handles.isEmpty()) kv.append(handles);
        keys.append(kv);
    }
    if (!keys.isEmpty()) o["keys"] = keys; // an oscillator keeps the curve it had, should it go back to it
}

static void trackFromJson(AnimTrack &t, const QJsonObject &o, double duration)
{
    t.enabled = o.value("enable").toBool(true);
    if (o.contains("oscillator")) {
        const QJsonObject w = o.value("oscillator").toObject();
        t.oscillator = true;
        t.wave = AnimWave(keyIndex(kWaveKeys, kAnimWaveCount, w.value("wave").toString(), 0));
        t.period = std::clamp(w.value("period").toDouble(1), 0.01, 36000.0);
        t.center = w.value("center").toDouble(0.5);
        t.amplitude = w.value("amplitude").toDouble(0.5);
        t.phase = w.value("phase").toDouble(0);
        t.seed = quint32(std::clamp(w.value("seed").toDouble(1), 0.0, 4294967295.0));
    }
    t.keys.clear();
    for (const QJsonValue &kv : o.value("keys").toArray()) {
        const QJsonArray k = kv.toArray();
        if (k.size() < 2) continue;
        AnimKey key;
        key.t = std::clamp(k[0].toDouble(), 0.0, duration);
        key.v = k[1].toDouble();
        key.curve = 3;
        for (int i = 2; i < k.size(); ++i) {
            const QJsonValue x = k[i];
            if (x.isDouble()) key.curve = std::clamp(x.toInt(3), 0, 5);
            else if (x.toString() == "hold") key.curve = kAnimHold;
            else if (x.toString() == "bezier") key.curve = kAnimBezier;
            else if (x.toString() == "current") key.isCurrentValue = t.keys.empty(); // the first key only
            else if (x.isObject()) {
                const QJsonArray in = x.toObject().value("in").toArray(), out = x.toObject().value("out").toArray();
                if (in.size() == 2) key.inDt = std::min(0.0, in[0].toDouble()), key.inDv = in[1].toDouble();
                if (out.size() == 2) key.outDt = std::max(0.0, out[0].toDouble()), key.outDv = out[1].toDouble();
            }
        }
        t.keys.push_back(key);
    }
    sortKeys(t);
}

// The transport settings shared by a timeline and a layer's animation
static void animToJson(const Animation &a, QJsonObject &o)
{
    o["duration"] = a.duration;
    o["loop"] = animLoopKey(a.loop);
    if (a.repeat > 0) o["repeat"] = a.repeat;
    if (a.speed != 1.0) o["speed"] = a.speed;
}

static void animFromJson(Animation &a, const QJsonObject &o)
{
    a.duration = std::clamp(o.value("duration").toDouble(4), kMinDuration, 36000.0);
    a.loop = animLoopFromKey(o.value("loop").toString());
    a.repeat = std::clamp(o.value("repeat").toInt(0), 0, 100000);
    a.speed = std::clamp(o.value("speed").toDouble(1.0), 0.0, 10.0);
}

QJsonObject layerAnimToJson(const Animation &a)
{
    QJsonObject o;
    if (a.tracks.empty()) return o;
    o["param"] = a.tracks.front().param;
    animToJson(a, o);
    trackToJson(a.tracks.front(), o);
    if (a.pinned) o["pinned"] = true;
    if (a.folded) o["folded"] = true;
    return o;
}

Animation layerAnimFromJson(const QJsonObject &o, quint64 layer)
{
    Animation a;
    animFromJson(a, o);
    a.pinned = o.value("pinned").toBool(false);
    a.folded = o.value("folded").toBool(false);
    AnimTrack t;
    t.layer = layer;
    t.param = o.value("param").toString();
    trackFromJson(t, o, a.duration);
    a.tracks.push_back(t);
    return a;
}

QJsonArray Engine::animationsToJson() const
{
    QJsonArray out;
    for (const Animation &a : m_animations) {
        QJsonArray tracks;
        for (const AnimTrack &t : a.tracks) {
            QJsonObject o{{"layer", QString::number(t.layer)}, {"param", t.param}};
            trackToJson(t, o);
            tracks.append(o);
        }
        QJsonObject o{{"id", QString::number(a.id)}, {"name", a.name}, {"tracks", tracks}};
        animToJson(a, o);
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
        animFromJson(a, o);
        for (const QJsonValue &tv : o.value("tracks").toArray()) {
            const QJsonObject to = tv.toObject();
            AnimTrack t;
            t.layer = to.value("layer").toString().toULongLong();
            t.param = to.value("param").toString();
            trackFromJson(t, to, a.duration);
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
