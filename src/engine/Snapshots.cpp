// Snapshots (cues): snapshots of the layers, recalled with a fade; another source comes in with a transition.
#include "EngineInternal.h"
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
    return timingKey == QLatin1String("text/content") ? QStringLiteral("linear") : QStringLiteral("ease_in_out");
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

// The key of a snapshot's "timing" (or "easing") that gives a value of a layer its time (its easing): its own address,
// or the address of a group it is in ("position" for "position/x", "fx/blur" for all of that effect's values); empty
// when none does (it follows the snapshot's fade, with the default easing)
QString Engine::timingKeyOf(const QJsonObject &timing, const QString &path)
{
    for (QString key = path; !key.isEmpty();) {
        if (timing.contains(key)) return key;
        const int cut = key.lastIndexOf('/');
        if (cut <= 0) break;
        key.truncate(cut);
    }
    return {};
}

static double timeOf(const QJsonObject &timing, const QString &path, double fade)
{
    const QJsonValue v = timing.value(Engine::timingKeyOf(timing, path));
    return v.isDouble() ? std::clamp(v.toDouble(), 0.0, 600.0) : std::max(0.0, fade);
}

static EasingCurve easingOf(const QJsonObject &easing, const QString &path)
{
    const QString k = Engine::timingKeyOf(easing, path);
    return easingCurveFromKey(k.isEmpty() ? Engine::defaultEasing(path) : easing.value(k).toString());
}

// One number of a layer on its way to a snapshot's, in its own time
struct FadeTrack {
    QString path;
    ParamInfo::Ramp ramp = ParamInfo::Ramp::Linear;
    double from = 0, to = 0, time = 0;
    EasingCurve curve = EasingCurve::EaseInOut;
};

// A layer on its way to a snapshot's state. Several snapshots can run at once: a new recall takes over only the values
// it holds, the others go on with the snapshot that started them. Its parameters fade each on its own (how, their ramp
// says: a switch, a choice, a text are set at once); its routing in the viewports and its mesh's points as a whole.
struct Engine::FadeJob {
    quint64 id = 0;
    std::vector<FadeTrack> tracks;
    bool routing = false; // how much of it each viewport shows
    std::map<quint64, float> routeFrom, routeTo;
    double routeTime = 0;
    EasingCurve routeCurve = EasingCurve::EaseInOut;
    bool mesh = false; // its mesh's points (same grid)
    std::vector<QPointF> meshFrom, meshTo;
    double meshTime = 0;
    EasingCurve meshCurve = EasingCurve::EaseInOut;
    double elapsed = 0;  // its own clock: snapshots started at different times run side by side
    bool hideAtEnd = false; // faded out, then hidden once its opacity got there
    float finalOpacity = 1;
    double hideTime = 0;
    bool opacityReleased = false; // an animation drives its opacity: left where it is when it is hidden

    double longest() const
    {
        double m = hideAtEnd ? hideTime : 0.0;
        for (const FadeTrack &t : tracks) m = std::max(m, t.time);
        if (routing) m = std::max(m, routeTime);
        if (mesh) m = std::max(m, meshTime);
        return m;
    }
    bool done() const { return tracks.empty() && !routing && !mesh && !hideAtEnd; }
    // The values another recall now sets: no longer this one's
    void drop(const QSet<QString> &paths)
    {
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const FadeTrack &t) { return paths.contains(t.path); }),
                     tracks.end());
        if (paths.contains(QStringLiteral("viewports"))) routing = false;
        if (paths.contains(QStringLiteral("mesh"))) mesh = false;
        if (paths.contains(QStringLiteral("opacity"))) hideAtEnd = false;
    }
};

static double mixd(double a, double b, double t) { return a + (b - a) * t; }
static float mixf(float a, float b, double t) { return float(a + (b - a) * t); }

// Progress of a number `elapsed` seconds into its own time: 1 for a cut, eased with given curve otherwise
static double progress(double elapsed, double duration, EasingCurve curve = EasingCurve::EaseInOut)
{
    if (duration <= 0) return 1.0;
    const double t = std::min(1.0, elapsed / duration);
    return applyEasing(t, curve);
}

// Where a track is `elapsed` seconds in: its target once its time is over (an angle: as stored, not a turn more)
static double trackValue(const FadeTrack &t, double elapsed)
{
    if (elapsed >= t.time) return t.to;
    const double k = progress(elapsed, t.time, t.curve);
    if (t.ramp == ParamInfo::Ramp::Angle) { // the shortest way round
        double d = std::fmod(t.to - t.from, 360.0);
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return t.from + d * k;
    }
    return mixd(t.from, t.to, k);
}

// Sets a job's values where it is now; what is over goes (lock held)
static void stepJob(Engine::FadeJob &job, Layer &l)
{
    const double e = job.elapsed;
    for (const FadeTrack &t : job.tracks)
        if (Parameter *p = l.parameter(t.path)) p->setNumber(trackValue(t, e));
    job.tracks.erase(std::remove_if(job.tracks.begin(), job.tracks.end(), [e](const FadeTrack &t) { return e >= t.time; }),
                     job.tracks.end());
    if (job.routing) {
        const double k = progress(e, job.routeTime, job.routeCurve);
        std::map<quint64, float> now;
        auto at = [](const std::map<quint64, float> &m, quint64 v) { // a viewport not in the map shows it fully
            const auto it = m.find(v);
            return it == m.end() ? 1.0f : it->second;
        };
        for (const auto &[v, a] : job.routeFrom) now[v] = 0;
        for (const auto &[v, a] : job.routeTo) now[v] = 0;
        for (auto &[v, a] : now) a = mixf(at(job.routeFrom, v), at(job.routeTo, v), k);
        if (e >= job.routeTime) {
            now = job.routeTo;
            job.routing = false;
        }
        l.viewportOpacity = now;
    }
    if (job.mesh && l.mapping.offsets.size() == job.meshTo.size()) {
        const double k = progress(e, job.meshTime, job.meshCurve);
        for (size_t i = 0; i < job.meshTo.size(); ++i) l.mapping.offsets[i] = job.meshFrom[i] + (job.meshTo[i] - job.meshFrom[i]) * k;
        ++l.mapping.revision;
        if (e >= job.meshTime) job.mesh = false;
    } else {
        job.mesh = false;
    }
    if (job.hideAtEnd && e >= job.hideTime) {
        l.enabled = false;
        if (!job.opacityReleased)
            if (Parameter *p = l.parameter(QStringLiteral("opacity"))) p->setNumber(job.finalOpacity); // stays as stored
        job.hideAtEnd = false;
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
    const QJsonObject easing = c.value("easing").toObject();
    auto curve = [&](const QString &key) { return int(easingOf(easing, key)); };
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
            if (job->elapsed <= 0) total = std::max(total, job->longest()); // this recall's, not the older ones
        for (const auto &[id, t] : m_transitions) total = std::max(total, t->duration - t->elapsed);
        if (m_compFade.opacity && m_compFade.opacityElapsed <= 0) total = std::max(total, m_compFade.opacityDur);
        if (m_compFade.volume && m_compFade.volumeElapsed <= 0) total = std::max(total, m_compFade.volumeDur);
        m_recallTotal = total;
    }
    emit snapshotRecalled(i);
}

void Engine::applyLayers(const QJsonArray &layers, double fade, bool hideOthers)
{
    fade = std::max(0.0, fade);
    QSet<quint64> named; // the layers the state speaks of (left out or not)
    for (const QJsonValue &v : layers) named.insert(v.toObject().value("id").toString().toULongLong());
    // The snapshots still running go on: this recall stops their fades only for the values it sets itself
    // (from where they are now), and leaves them the others
    auto takeOver = [this](quint64 id, const QSet<QString> &paths, bool all = false) {
        Lock lk(&m_mutex);
        for (auto it = m_fades.begin(); it != m_fades.end();) {
            FadeJob &j = **it;
            if (j.id == id) {
                if (all) j = FadeJob{};
                else j.drop(paths);
            }
            it = j.done() ? m_fades.erase(it) : it + 1;
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
            takeOver(id, {}, true);
            insertLayerJson(layerCount(), o);
            continue;
        }
        const QJsonObject timing = o.value("timing").toObject(), easing = o.value("easing").toObject();
        const QJsonObject params = o.value("params").toObject();
        const QJsonObject cur = layerJson(idx);
        const QJsonObject curSrc = cur.value("source").toObject(), src = o.value("source").toObject();
        const bool group = o.value("group").toBool();
        auto job = std::make_shared<FadeJob>();
        job->id = id;
        // A number on its way from `from` to the snapshot's, in its time; at once when it has none
        auto fadeTo = [&](Parameter &p, double from, double to) {
            const ParamInfo info = p.info();
            const double t = timeOf(timing, info.path, fade);
            if (t > 0 && std::abs(to - from) > 1e-12) {
                job->tracks.push_back({info.path, info.ramp, from, to, t, easingOf(easing, info.path)});
                p.setNumber(from);
            } else {
                p.setNumber(to);
            }
        };
        if (!group && (curSrc.value("type") != src.value("type") || curSrc.value("layer") != src.value("layer") ||
                       curSrc.value("tap") != src.value("tap") ||
                       QDir::cleanPath(curSrc.value("path").toString()) != QDir::cleanPath(src.value("path").toString()))) {
            // Another media: it comes in with the layer's transition over the source's time (the snapshot's
            // fade unless it has its own), or at once for a cut
            const double t = timeOf(timing, QStringLiteral("file"), fade);
            takeOver(id, {}, true);
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
            // The layer now holds the snapshot's state; where it is, how it looks and its level move there from the
            // outgoing one's over their times, as they would with the same source
            Lock lk(&m_mutex);
            Layer *l = layer(indexOfId(id));
            const auto tr = m_transitions.find(id);
            if (!l || tr == m_transitions.end()) continue;
            Layer &old = *tr->second->from;
            for (Parameter *p : l->parameters()) {
                const QString &path = p->path();
                const bool carried = (partOf(path) & (PartRoi | PartColor | PartSpatial | PartCompositing)) || path == "volume";
                Parameter *q = carried && p->isNumber() && p->info().ramp != ParamInfo::Ramp::Cut ? old.parameter(path) : nullptr;
                if (!q) continue;
                fadeTo(*p, path == "opacity" && !old.enabled ? 0.0 : q->number(), p->number());
            }
            job->routing = true;
            job->routeFrom = old.viewportOpacity;
            job->routeTo = l->viewportOpacity;
            job->routeTime = timeOf(timing, QStringLiteral("viewports"), fade);
            job->routeCurve = easingOf(easing, QStringLiteral("viewports"));
            if (old.mapping.cols == l->mapping.cols && old.mapping.rows == l->mapping.rows) {
                job->mesh = true;
                job->meshFrom = old.mapping.offsets;
                job->meshTo = l->mapping.offsets;
                job->meshTime = timeOf(timing, QStringLiteral("mesh"), fade);
                job->meshCurve = easingOf(easing, QStringLiteral("mesh"));
            }
            stepJob(*job, *l);
            if (!job->done()) jobs.push_back(job);
            continue;
        }
        // Another chain of effects: at once, with its values
        const bool chain = effectPaths(cur.value("fx").toArray()) != effectPaths(o.value("fx").toArray());
        if (chain) setEffectsJson(idx, o.value("fx").toArray());

        // The values this state sets: their fades from other snapshots stop here
        QSet<QString> owned;
        for (auto it = params.constBegin(); it != params.constEnd(); ++it) owned.insert(it.key());
        owned << QStringLiteral("viewports") << QStringLiteral("opacity");
        if (o.contains("mesh")) owned << QStringLiteral("mesh");
        takeOver(id, owned);

        Lock lk(&m_mutex);
        Layer *l = layer(idx);
        if (!l) continue;
        l->name = o.value("name").toString(l->name);
        if (!group) l->color.maskLayer = o.value("mask").toString().toULongLong();
        const QJsonArray fx = o.value("fx").toArray();
        for (size_t k = 0; k < l->effects.size() && int(k) < fx.size(); ++k) { // their masks
            const QJsonObject e = fx[int(k)].toObject();
            l->effects[k]->maskLayer = e.value("mask").toString().toULongLong();
            l->effects[k]->maskPreFx = e.value("mask_tap").toString() == "prefx";
        }
        // Visibility: shown at once and faded in from 0, or faded out then hidden
        const bool visible = o.value("enable").toBool(true);
        const bool showing = visible && !l->enabled, hiding = !visible && l->enabled;
        if (showing) l->enabled = true;
        // Its parameters, in their order: the numbers fade (their ramp), the rest is set at once
        for (Parameter *p : l->parameters()) {
            const auto it = params.constFind(p->path());
            if (it == params.constEnd() || !p->isStored()) continue;
            const ParamInfo info = p->info();
            const QString &path = info.path;
            if (info.ramp == ParamInfo::Ramp::Typed) { // a text typed in over its time
                TextSource &t = l->text;
                const QString target = it.value().toString();
                if (path != "text/content" || target == t.content) {
                    p->setJson(it.value());
                    continue;
                }
                const double dur = timeOf(timing, path, fade);
                const QString from = t.shown(); // what is on screen now (a recall may interrupt another typing)
                t.content = target;
                t.stopTyping();
                if (dur > 0) {
                    t.typedFrom = from;
                    t.typeDur = dur;
                    t.typeProgress = 0;
                    t.typeCurve = int(easingOf(easing, path));
                }
                continue;
            }
            const bool cut = !p->isNumber() || info.ramp == ParamInfo::Ramp::Cut || (chain && path.startsWith("fx/"));
            if (path == "opacity") {
                const double target = it.value().toDouble(p->number());
                if (hiding) {
                    job->hideAtEnd = true;
                    job->finalOpacity = float(target);
                    job->hideTime = timeOf(timing, path, fade);
                }
                fadeTo(*p, showing ? 0.0 : p->number(), hiding ? 0.0 : target);
                continue;
            }
            if (cut) p->setJson(it.value());
            else fadeTo(*p, p->number(), it.value().toDouble(p->number()));
        }
        if (hiding && !params.contains("opacity")) { // no level of its own: faded out from where it is
            job->hideAtEnd = true;
            job->finalOpacity = l->opacity;
            job->hideTime = timeOf(timing, QStringLiteral("opacity"), fade);
            if (Parameter *p = l->parameter(QStringLiteral("opacity"))) fadeTo(*p, p->number(), 0.0);
        }
        // Which viewports it is drawn in: a snapshot can send a layer to another projector
        job->routing = true;
        job->routeFrom = l->viewportOpacity;
        job->routeTo.clear();
        const QJsonObject vo = o.value("viewports").toObject();
        for (auto v = vo.begin(); v != vo.end(); ++v) job->routeTo[v.key().toULongLong()] = float(std::clamp(v.value().toDouble(1.0), 0.0, 1.0));
        job->routeTime = timeOf(timing, QStringLiteral("viewports"), fade);
        job->routeCurve = easingOf(easing, QStringLiteral("viewports"));
        // The mesh's points: they move there on the same grid, another grid is set at once
        if (o.contains("mesh") && !l->isViewport) {
            Mapping target = l->mapping;
            target.setMeshJson(o.value("mesh").toObject());
            l->mapping.meshMode = target.meshMode;
            if (target.cols == l->mapping.cols && target.rows == l->mapping.rows) {
                job->mesh = true;
                job->meshFrom = l->mapping.offsets;
                job->meshTo = target.offsets;
                job->meshTime = timeOf(timing, QStringLiteral("mesh"), fade);
                job->meshCurve = easingOf(easing, QStringLiteral("mesh"));
            } else {
                l->mapping.setMeshJson(o.value("mesh").toObject());
            }
        }
        stepJob(*job, *l);
        if (!job->done()) jobs.push_back(job);
    }
    fixLayerReferences(); // layers re-created or sources changed: no reference left dangling or looping
    Lock lk(&m_mutex);
    // The layers the snapshot does not know (created since): faded out with the snapshot's fade, then hidden
    if (hideOthers)
        for (auto &lp : m_layers) {
            Layer &l = *lp;
            if (l.isViewport || named.contains(l.id) || !l.enabled || isLocked(indexOfId(l.id))) continue;
            takeOver(l.id, {QStringLiteral("opacity")}); // only its opacity: what other snapshots fade on it goes on
            if (fade <= 0) {
                l.enabled = false;
                continue;
            }
            auto job = std::make_shared<FadeJob>();
            job->id = l.id;
            job->tracks.push_back({QStringLiteral("opacity"), ParamInfo::Ramp::Linear, l.opacity, 0.0, fade, EasingCurve::EaseInOut});
            job->finalOpacity = l.opacity;
            job->hideAtEnd = true;
            job->hideTime = fade;
            jobs.push_back(job);
        }
    for (auto &j : jobs) m_fades.push_back(std::move(j));
    m_fadeElapsed = 0;
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
        stepJob(job, *l);
        it = job.done() ? m_fades.erase(it) : it + 1;
    }
}

// What an animation drives stays as it sets it: the fades leave that value to it
void Engine::releaseFromFades(quint64 layer, const QString &path)
{
    for (const auto &job : m_fades) {
        if (job->id != layer) continue;
        auto &t = job->tracks;
        t.erase(std::remove_if(t.begin(), t.end(), [&](const FadeTrack &x) { return x.path == path; }), t.end());
        if (path == QLatin1String("opacity")) job->opacityReleased = true;
    }
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
