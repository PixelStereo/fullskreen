#pragma once
// Undo commands (QUndoStack). Each command is applied to the engine under its lock.
// Layers are identified by index: the stack guarantees that index order is consistent
// at the time a command is undone or redone.

#include "Engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoCommand>
#include <QVariant>

namespace cmd {

enum Id { ParamId = 1, PropId = 2, NudgeId = 3 };

// A layer's ISF instance: slot -1 = generator, otherwise the effect index. Engine lock required.
IsfInstance *resolveIsf(Engine *e, int layer, int slot);

// ISF shader parameter (merges continuous moves of the same slider)
class SetParam : public QUndoCommand
{
public:
    SetParam(Engine *e, int layer, int slot, int input, const IsfValue &before, const IsfValue &after, const QString &label);
    void undo() override;
    void redo() override;
    int id() const override { return ParamId; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(const IsfValue &v);
    Engine *m_e;
    int m_layer, m_slot, m_input;
    IsfValue m_before, m_after;
    qint64 m_time;
};

// Simple layer property
class SetLayerProp : public QUndoCommand
{
public:
    enum Prop { Name, Visible, Opacity, Blend, Speed, Loop, Volume, Muted };
    SetLayerProp(Engine *e, int layer, Prop prop, const QVariant &before, const QVariant &after);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return PropId; }
    bool mergeWith(const QUndoCommand *other) override;
    static QVariant read(Engine *e, int layer, Prop prop);

private:
    void apply(const QVariant &v);
    Engine *m_e;
    int m_layer;
    Prop m_prop;
    QVariant m_before, m_after;
    qint64 m_time;
};

// Full mapping (corners + mesh) before / after an edit
class SetMapping : public QUndoCommand
{
public:
    SetMapping(Engine *e, int layer, const Mapping &before, const Mapping &after, const QString &text, bool nudge = false);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return m_nudge ? NudgeId : -1; }
    bool mergeWith(const QUndoCommand *other) override;
    static Mapping read(Engine *e, int layer);

private:
    void apply(const Mapping &m);
    Engine *m_e;
    int m_layer;
    Mapping m_before, m_after;
    bool m_nudge;
    qint64 m_time;
};

// Layer added (already done at push time)
class AddLayer : public QUndoCommand
{
public:
    AddLayer(Engine *e, int index, const QString &text);
    void undo() override;
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonObject m_json;
    bool m_first = true;
};

// Layer deleted
class RemoveLayer : public QUndoCommand
{
public:
    RemoveLayer(Engine *e, int index);
    void undo() override;
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonObject m_json;
};

class MoveLayer : public QUndoCommand
{
public:
    MoveLayer(Engine *e, int from, int to);
    void undo() override { m_e->moveLayer(m_to, m_from); }
    void redo() override { m_e->moveLayer(m_from, m_to); }

private:
    Engine *m_e;
    int m_from, m_to;
};

// Layer replaced by another full state (source change…); already done at push time
class ReplaceLayer : public QUndoCommand
{
public:
    ReplaceLayer(Engine *e, int index, const QJsonObject &before, const QString &text);
    void undo() override { m_e->replaceLayerJson(m_index, m_before); }
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonObject m_before, m_after;
    bool m_first = true;
};

// Effect chain before / after (add, remove, reorder, enable); already done at push time
class SetEffects : public QUndoCommand
{
public:
    SetEffects(Engine *e, int index, const QJsonArray &before, const QString &text);
    void undo() override { m_e->setEffectsJson(m_index, m_before); }
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonArray m_before, m_after;
    bool m_first = true;
};

} // namespace cmd
