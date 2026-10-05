// Sequences: ordered steps recalling memories, played with GO / GO BACK (the cue list of a show).
#include "Engine.h"

#include <QJsonArray>
#include <QThread>
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

double Engine::memoryDuration(quint64 id) const
{
    Lock lk(&m_mutex);
    const Memory *m = nullptr;
    for (const Memory &x : m_memories)
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
        StepRun r;
        r.step = step;
        r.memory = st.memory;
        r.preWait = chain ? std::max(0.0, st.preWait) : 0.0;
        r.postWait = std::max(0.0, st.postWait);
        r.next = chain ? st.next : StepContinue::Wait;
        r.duration = memoryDuration(st.memory); // the lock is recursive
        m_runs.push_back(r);
        m_sequencePosition = step; // the playhead moves at the GO, the memory comes after the pre-wait
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
            const double dt = m_sequenceClock.nsecsElapsed() * 1e-9;
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

// Each step on its way moves on by dt: its memory once its pre-wait is over, the next step's GO when its
// continuation says so (the time beyond it carried over, so that chained waits do not drift). A step is
// forgotten once its action is over and nothing more comes from it.
void Engine::advanceSequence(double dt)
{
    dt = std::max(0.0, dt);
    bool moved = false;
    for (int round = 0; round < 256; ++round) { // steps without any wait follow each other in the same call
        std::vector<quint64> fire;
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
                    fire.push_back(r.memory);
                }
                const double at = r.continueAt();
                if (!r.fired || r.continued || at < 0 || r.elapsed < at) continue;
                r.continued = true;
                const int k = r.step + 1 < n ? r.step + 1 : s.loop ? 0 : -1;
                if (k < 0) continue;
                const SequenceStep &st = s.steps[size_t(k)];
                StepRun x;
                x.step = k;
                x.memory = st.memory;
                x.elapsed = r.elapsed - at;
                x.preWait = std::max(0.0, st.preWait);
                x.postWait = std::max(0.0, st.postWait);
                x.next = st.next;
                x.duration = memoryDuration(st.memory);
                started.push_back(x);
            }
            m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(),
                                        [](const StepRun &r) {
                                            return r.fired && (r.next == StepContinue::Wait || r.continued) &&
                                                   r.elapsed >= r.preWait + r.duration;
                                        }),
                         m_runs.end());
        }
        // The memories, in the order of their GO (outside the lock: a recall loads, and may go through the
        // interface's undo stack)
        for (quint64 id : fire) {
            const int mi = indexOfMemory(id);
            if (mi < 0) continue; // a step without its memory (gone): the position moves, nothing else
            if (m_recaller) m_recaller(mi);
            else recallMemory(mi);
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
            QJsonObject o{{"memory", QString::number(st.memory)}};
            if (!st.text.isEmpty()) o["text"] = st.text;
            if (st.preWait > 0) o["preWait"] = st.preWait;
            if (st.postWait > 0) o["postWait"] = st.postWait;
            if (st.next != StepContinue::Wait) o["continue"] = st.next == StepContinue::Follow ? "follow" : "autoFollow";
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
            st.memory = so.value("memory").toString().toULongLong();
            st.text = so.value("text").toString();
            st.preWait = std::clamp(so.value("preWait").toDouble(0), 0.0, 3600.0);
            st.postWait = std::clamp(so.value("postWait").toDouble(0), 0.0, 3600.0);
            const QString c = so.value("continue").toString();
            st.next = c == "follow" ? StepContinue::Follow : c == "autoFollow" ? StepContinue::AutoFollow : StepContinue::Wait;
            s.steps.push_back(st);
        }
        m_sequences.push_back(s);
    }
    m_currentSequence = m_sequences.empty() ? -1 : std::clamp(current, 0, int(m_sequences.size()) - 1);
    m_sequencePosition = -1;
    m_runs.clear();
}
