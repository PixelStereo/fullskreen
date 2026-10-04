// Sequences: ordered steps recalling memories, played with GO / GO BACK (the cue list of a show).
#include "Engine.h"

#include <QJsonArray>

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

bool Engine::sequenceGoTo(int step)
{
    quint64 mem = 0;
    {
        Lock lk(&m_mutex);
        if (m_currentSequence < 0) return false;
        const Sequence &s = m_sequences[size_t(m_currentSequence)];
        if (step < 0 || step >= int(s.steps.size())) return false;
        mem = s.steps[size_t(step)].memory;
        m_sequencePosition = step;
    }
    emit sequencePositionChanged();
    const int mi = indexOfMemory(mem);
    if (mi >= 0) recallMemory(mi); // a step without its memory (gone): the position moves, nothing else
    return true;
}

bool Engine::sequenceGo() { return sequenceGoTo(sequenceNext()); }
bool Engine::sequenceBack() { return sequenceGoTo(sequencePrevious()); }

QJsonArray Engine::sequencesToJson() const
{
    QJsonArray out;
    for (const Sequence &s : m_sequences) {
        QJsonArray steps;
        for (const SequenceStep &st : s.steps) {
            QJsonObject o{{"memory", QString::number(st.memory)}};
            if (!st.text.isEmpty()) o["text"] = st.text;
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
            s.steps.push_back({so.value("memory").toString().toULongLong(), so.value("text").toString()});
        }
        m_sequences.push_back(s);
    }
    m_currentSequence = m_sequences.empty() ? -1 : std::clamp(current, 0, int(m_sequences.size()) - 1);
    m_sequencePosition = -1;
}
