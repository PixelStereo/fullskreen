// Sequences: ordered steps recalling snapshots, played with GO / GO BACK (the cue list of a show).
#include <limits>
#include "EngineInternal.h"

#include <QJsonArray>
#include <QThread>
#include <cmath>
#include <QTimer>

int Engine::sequenceCount() const
{
    Lock lk(&m_mutex);
    return int(m_sequences.size());
}

Engine::Sequence Engine::sequence(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_sequences.size()) ? m_sequences[size_t(i)] : Sequence();
}

void Engine::setSequence(int i, const Sequence &s)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_sequences.size())) return;
        m_sequences[size_t(i)] = s;
        if (i == m_currentSequence && m_sequencePosition >= int(s.steps.size())) m_sequencePosition = int(s.steps.size()) - 1;
        if (i == m_currentSequence)
            m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(), [&](const StepRun &r) { return r.step >= int(s.steps.size()); }),
                         m_runs.end());
    }
    emit sequencesChanged();
}

int Engine::addSequence(const Sequence &s, int at)
{
    {
        Lock lk(&m_mutex);
        if (at < 0 || at > int(m_sequences.size())) at = int(m_sequences.size());
        m_sequences.insert(m_sequences.begin() + at, s);
        if (m_currentSequence >= at) ++m_currentSequence;
        if (m_currentSequence < 0) {
            m_currentSequence = at;
            m_sequencePosition = -1;
        }
    }
    emit sequencesChanged();
    emit sequencePositionChanged();
    return at;
}

void Engine::removeSequence(int i)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_sequences.size())) return;
        m_sequences.erase(m_sequences.begin() + i);
        m_runs.clear();
        if (m_currentSequence == i) {
            m_currentSequence = m_sequences.empty() ? -1 : std::min(i, int(m_sequences.size()) - 1);
            m_sequencePosition = -1;
        } else if (m_currentSequence > i) {
            --m_currentSequence;
        }
    }
    emit sequencesChanged();
    emit sequencePositionChanged();
}

int Engine::currentSequence() const
{
    Lock lk(&m_mutex);
    return m_currentSequence;
}

void Engine::setCurrentSequence(int i)
{
    {
        Lock lk(&m_mutex);
        if (i < -1 || i >= int(m_sequences.size()) || i == m_currentSequence) return;
        m_currentSequence = i;
        m_sequencePosition = -1;
        m_runs.clear(); // the waits of the other one: no step comes from them any more
    }
    emit sequencePositionChanged();
}

int Engine::sequencePosition() const
{
    Lock lk(&m_mutex);
    return m_sequencePosition;
}

void Engine::setSequencePosition(int step)
{
    {
        Lock lk(&m_mutex);
        if (m_currentSequence < 0) return;
        const int n = int(m_sequences[size_t(m_currentSequence)].steps.size());
        m_sequencePosition = std::clamp(step, -1, n - 1);
    }
    emit sequencePositionChanged();
}

int Engine::sequenceNext() const
{
    Lock lk(&m_mutex);
    if (m_currentSequence < 0) return -1;
    const Sequence &s = m_sequences[size_t(m_currentSequence)];
    const int n = int(s.steps.size());
    if (n == 0) return -1;
    if (m_sequencePosition + 1 < n) return m_sequencePosition + 1;
    return s.loop ? 0 : -1;
}

int Engine::sequencePrevious() const
{
    Lock lk(&m_mutex);
    if (m_currentSequence < 0) return -1;
    const Sequence &s = m_sequences[size_t(m_currentSequence)];
    const int n = int(s.steps.size());
    if (n == 0 || m_sequencePosition < 0) return -1;
    if (m_sequencePosition > 0) return m_sequencePosition - 1;
    return s.loop ? n - 1 : -1;
}

// ---------------------------------------------------------------------------
// Playing: pre-wait, action, post-wait, and what follows

double Engine::snapshotDuration(quint64 id) const
{
    Lock lk(&m_mutex);
    const Snapshot *m = nullptr;
    for (const Snapshot &x : m_snapshots)
        if (x.id == id) m = &x;
    if (!m) return 0;
    double d = 0;
    auto scan = [&](const QJsonObject &timing) {
        d = std::max(d, m->fade); // the values without a time of their own
        for (auto it = timing.begin(); it != timing.end(); ++it)
            if (it.value().isDouble()) d = std::max(d, std::clamp(it.value().toDouble(), 0.0, 600.0));
    };
    for (const QJsonValue &v : m->layers) {
        const QJsonObject o = v.toObject();
        if (o.value("included").toBool(true) && !o.value("viewport").toBool()) scan(o.value("timing").toObject());
    }
    if (!m->composition.isEmpty() && m->composition.value("included").toBool(true))
        scan(m->composition.value("timing").toObject());
    return d;
}

double Engine::stepDuration(const SequenceStep &st) const
{
    if (!st.timeline) return snapshotDuration(st.snapshot);
    if (st.action != AnimAction::Play) return 0;
    Lock lk(&m_mutex);
    for (const Animation &a : m_animations) {
        if (a.id != st.timeline) continue;
        const double len = a.length();
        if (!std::isfinite(len)) return 0; // endless: nothing to wait for
        const double v = a.speed * timeScale();
        if (v <= 1e-9) return std::numeric_limits<double>::infinity(); // frozen: waits for its speed
        return (a.state == AnimState::Stopped ? len : std::max(0.0, len - a.clock)) / v; // in real seconds
    }
    return 0;
}

// What a timeline that was told to play still has to play, in real seconds: 0 once it is over (or stopped,
// or gone, or endless: nothing to wait for), infinity while it does not move (paused, or speed 0)
double Engine::animationRemaining(quint64 id) const
{
    Lock lk(&m_mutex);
    for (const Animation &a : m_animations) {
        if (a.id != id) continue;
        const double len = a.length();
        if (a.state == AnimState::Stopped || !std::isfinite(len)) return 0;
        const double v = a.speed * timeScale();
        if (a.state == AnimState::Paused || v <= 1e-9) return std::numeric_limits<double>::infinity();
        return std::max(0.0, len - a.clock) / v;
    }
    return 0;
}

// A step's run, from its GO
static Engine::StepRun runOf(const Engine::SequenceStep &st, int k, bool chain)
{
    Engine::StepRun r;
    r.step = k;
    r.target = st;
    r.snapshot = st.timeline ? 0 : st.snapshot;
    r.preWait = chain ? std::max(0.0, st.preWait) : 0.0;
    r.postWait = std::max(0.0, st.postWait);
    r.next = chain ? st.next : Engine::StepContinue::Wait;
    return r;
}

bool Engine::sequenceGoTo(int step, bool chain)
{
    if (QThread::currentThread() != thread()) {
        // OSC: played on the engine's thread (the timer, the recaller and its undo stack live there)
        bool ok = false;
        {
            Lock lk(&m_mutex);
            ok = m_currentSequence >= 0 && step >= 0 && step < int(m_sequences[size_t(m_currentSequence)].steps.size());
        }
        if (ok) QMetaObject::invokeMethod(this, [this, step, chain] { sequenceGoTo(step, chain); }, Qt::QueuedConnection);
        return ok;
    }
    {
        Lock lk(&m_mutex);
        if (m_currentSequence < 0) return false;
        const Sequence &s = m_sequences[size_t(m_currentSequence)];
        if (step < 0 || step >= int(s.steps.size())) return false;
        const SequenceStep &st = s.steps[size_t(step)];
        if (!chain) m_runs.clear();
        StepRun r = runOf(st, step, chain);
        r.duration = stepDuration(st); // the lock is recursive
        m_runs.push_back(r);
        m_sequencePosition = step; // the playhead moves at the GO, the snapshot comes after the pre-wait
    }
    advanceSequence(0); // what has no pre-wait: now
    emit sequencePositionChanged();
    startSequenceTimer();
    return true;
}

bool Engine::sequenceGo() { return sequenceGoTo(sequenceNext()); }
bool Engine::sequenceBack() { return sequenceGoTo(sequencePrevious(), false); }

void Engine::sequenceStop()
{
    Lock lk(&m_mutex);
    m_runs.clear();
}

std::vector<Engine::StepRun> Engine::sequenceRuns() const
{
    Lock lk(&m_mutex);
    return m_runs;
}

bool Engine::sequenceRunning() const
{
    Lock lk(&m_mutex);
    for (const StepRun &r : m_runs)
        if (!r.fired || (r.next != StepContinue::Wait && !r.continued)) return true;
    return false;
}

void Engine::startSequenceTimer()
{
    if (m_fadesManual || m_runs.empty()) return;
    if (!m_sequenceTimer) {
        m_sequenceTimer = new QTimer(this);
        m_sequenceTimer->setTimerType(Qt::PreciseTimer);
        m_sequenceTimer->setInterval(5);
        connect(m_sequenceTimer, &QTimer::timeout, this, [this] {
            const double dt = m_sequenceClock.nsecsElapsed() * 1e-9 * timeScale();
            m_sequenceClock.restart();
            advanceSequence(dt);
            Lock lk(&m_mutex);
            if (m_runs.empty()) m_sequenceTimer->stop();
        });
    }
    if (!m_sequenceTimer->isActive()) {
        m_sequenceClock.restart();
        m_sequenceTimer->start();
    }
}

// Each step on its way moves on by dt: its snapshot once its pre-wait is over, the next step's GO when its
// continuation says so (the time beyond it carried over, so that chained waits do not drift). A step is
// forgotten once its action is over and nothing more comes from it.
void Engine::advanceSequence(double dt)
{
    dt = std::max(0.0, dt);
    bool moved = false;
    for (int round = 0; round < 256; ++round) { // steps without any wait follow each other in the same call
        std::vector<SequenceStep> fire;
        std::vector<StepRun> started;
        {
            Lock lk(&m_mutex);
            if (m_currentSequence < 0) {
                m_runs.clear();
                break;
            }
            const Sequence &s = m_sequences[size_t(m_currentSequence)];
            const int n = int(s.steps.size());
            for (StepRun &r : m_runs) {
                r.elapsed += dt;
                if (!r.fired && r.elapsed >= r.preWait) {
                    r.fired = true;
                    if (r.target.timeline) r.duration = stepDuration(r.target); // where the timeline is now
                    r.trackPending = r.target.timeline && r.target.action == AnimAction::Play;
                    fire.push_back(r.target);
                } else if (r.tracking) {
                    // The timeline plays on its own clock: its speed or loop mode may have changed since
                    r.duration = std::max(0.0, r.elapsed - r.preWait) + animationRemaining(r.target.timeline);
                }
                const double at = r.continueAt();
                if (!r.fired || r.continued || at < 0 || r.elapsed < at) continue;
                r.continued = true;
                const int k = r.step + 1 < n ? r.step + 1 : s.loop ? 0 : -1;
                if (k < 0) continue;
                const SequenceStep &st = s.steps[size_t(k)];
                StepRun x = runOf(st, k, true);
                x.elapsed = r.elapsed - at;
                x.duration = stepDuration(st);
                started.push_back(x);
            }
            m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(),
                                        [](const StepRun &r) {
                                            return r.fired && (r.next == StepContinue::Wait || r.continued) &&
                                                   r.elapsed >= r.preWait + r.duration;
                                        }),
                         m_runs.end());
        }
        // The snapshots and timelines, in the order of their GO (outside the lock: a recall loads, and may go
        // through the interface's undo stack)
        for (const SequenceStep &st : fire) {
            if (st.timeline) {
                controlAnimation(st.timeline, st.action, st.action == AnimAction::Speed ? st.speed : st.seekTime, st.loop, st.repeat);
                continue;
            }
            const int mi = indexOfSnapshot(st.snapshot);
            if (mi < 0) continue; // a step without its snapshot (gone): the position moves, nothing else
            if (m_recaller) m_recaller(mi);
            else recallSnapshot(mi);
        }
        {
            Lock lk(&m_mutex); // the Plays are issued: their steps follow the timelines from now on
            for (StepRun &r : m_runs)
                if (r.trackPending) r.trackPending = false, r.tracking = true;
        }
        if (started.empty()) break;
        {
            Lock lk(&m_mutex);
            for (StepRun &x : started) {
                m_runs.push_back(x);
                m_sequencePosition = x.step;
            }
        }
        moved = true;
        dt = 0; // the next round: only what the new steps carry over
    }
    if (moved) emit sequencePositionChanged();
}

QJsonArray Engine::sequencesToJson() const
{
    QJsonArray out;
    for (const Sequence &s : m_sequences) {
        QJsonArray steps;
        for (const SequenceStep &st : s.steps) {
            QJsonObject o;
            if (st.timeline) {
                static const char *const actions[] = {"play", "pause", "stop", "rewind", "seek", "loop_mode", "speed"};
                o["timeline"] = QString::number(st.timeline);
                o["action"] = QString::fromLatin1(actions[std::clamp(int(st.action), 0, 6)]);
                if (st.action == AnimAction::Seek) o["time"] = st.seekTime;
                if (st.action == AnimAction::Speed) o["speed"] = st.speed;
                if (st.action == AnimAction::LoopMode) {
                    o["loop"] = animLoopKey(st.loop);
                    if (st.repeat > 0) o["repeat"] = st.repeat;
                }
            } else {
                o["snapshot"] = QString::number(st.snapshot);
            }
            if (!st.text.isEmpty()) o["text"] = st.text;
            if (st.preWait > 0) o["pre_wait"] = st.preWait;
            if (st.postWait > 0) o["post_wait"] = st.postWait;
            if (st.next != StepContinue::Wait) o["continue"] = st.next == StepContinue::Follow ? "follow" : "auto_follow";
            steps.append(o);
        }
        out.append(QJsonObject{{"name", s.name}, {"loop", s.loop}, {"steps", steps}});
    }
    return out;
}

void Engine::sequencesFromJson(const QJsonArray &a, int current)
{
    m_sequences.clear();
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        Sequence s;
        s.name = o.value("name").toString();
        s.loop = o.value("loop").toBool(false);
        for (const QJsonValue &sv : o.value("steps").toArray()) {
            const QJsonObject so = sv.toObject();
            SequenceStep st;
            st.snapshot = so.value("snapshot").toString().toULongLong();
            st.timeline = so.value("timeline").toString().toULongLong();
            if (st.timeline) {
                const QString a = so.value("action").toString();
                st.action = a == "pause" ? AnimAction::Pause : a == "stop" ? AnimAction::Stop : a == "rewind" ? AnimAction::Rewind
                          : a == "seek" ? AnimAction::Seek : a == "loop_mode" ? AnimAction::LoopMode
                          : a == "speed" ? AnimAction::Speed : AnimAction::Play;
                st.speed = std::clamp(so.value("speed").toDouble(1), 0.0, 10.0);
                st.seekTime = std::max(0.0, so.value("time").toDouble(0));
                st.loop = animLoopFromKey(so.value("loop").toString());
                st.repeat = std::clamp(so.value("repeat").toInt(0), 0, 100000);
                st.snapshot = 0;
            }
            st.text = so.value("text").toString();
            st.preWait = std::clamp(so.value("pre_wait").toDouble(0), 0.0, 3600.0);
            st.postWait = std::clamp(so.value("post_wait").toDouble(0), 0.0, 3600.0);
            const QString c = so.value("continue").toString();
            st.next = c == "follow" ? StepContinue::Follow : c == "auto_follow" ? StepContinue::AutoFollow : StepContinue::Wait;
            s.steps.push_back(st);
        }
        m_sequences.push_back(s);
    }
    m_currentSequence = m_sequences.empty() ? -1 : std::clamp(current, 0, int(m_sequences.size()) - 1);
    m_sequencePosition = -1;
    m_runs.clear();
}
