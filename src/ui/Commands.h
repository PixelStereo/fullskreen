#pragma once
// Undo commands (QUndoStack). Each command is applied to the engine under its lock.
// Layers are identified by index: the stack guarantees that index order is consistent
// at the time a command is undone or redone.

#include "Engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoCommand>
#include <QVariant>
#include <optional>

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

// Speed of the time of an ISF instance (generator or effect; merges continuous moves)
class SetIsfSpeed : public QUndoCommand
{
public:
    SetIsfSpeed(Engine *e, int layer, int slot, double before, double after);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return 9301; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(double v);
    Engine *m_e;
    int m_layer, m_slot;
    double m_before, m_after;
    qint64 m_time;
};

// Simple layer property
class SetLayerProp : public QUndoCommand
{
public:
    enum Prop { Name, Enabled, Opacity, Blend, Speed, Mode, Volume, Muted, InPoint, OutPoint,
                ColorOn, TempOn, TintOn, AddOn, RemoveOn, Locked, EffectsEnabled,
                ColorAdd, ColorRemove, Roi, Temp, Tint, Transition, ColorMask, ColorMaskInvert };
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

// Order and grouping of the layers (moves, groups); applied by redo
class SetStructure : public QUndoCommand
{
public:
    SetStructure(Engine *e, const LayerTree &before, const LayerTree &after, const QString &text);
    void undo() override { m_e->setStructure(m_before); }
    void redo() override { m_e->setStructure(m_after); }

private:
    Engine *m_e;
    LayerTree m_before, m_after;
};

// Snapshot recalled: the layers' states before, then the snapshot's (with its fade the first time)
class RecallSnapshot : public QUndoCommand
{
public:
    RecallSnapshot(Engine *e, int snapshot);
    void undo() override
    {
        m_e->applyLayers(m_before, 0);
        m_e->applyComposition(m_beforeComposition, 0);
    }
    void redo() override;

private:
    Engine *m_e;
    int m_snapshot;
    QJsonArray m_before;
    QJsonObject m_beforeComposition;
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

// How much of an item at the top of the list a viewport shows (by ids: the list may change in between).
// Dragging the slider is one step: the same item and viewport merge.
class SetOpacityIn : public QUndoCommand
{
public:
    SetOpacityIn(Engine *e, quint64 layer, quint64 viewport, float before, float after, const QString &text);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return 9107; }
    bool mergeWith(const QUndoCommand *o) override;

private:
    void apply(float v);
    Engine *m_e;
    quint64 m_layer, m_viewport;
    float m_before, m_after;
};

// A timeline (animation) edited: tracks, keys, duration, loop, repeat, speed, name. By id (the list may change).
// Edits with the same non-empty merge key that follow one another (a key dragged, the arrows of a number) are one step.
class SetAnimation : public QUndoCommand
{
public:
    SetAnimation(Engine *e, quint64 id, const Engine::Animation &before, const Engine::Animation &after, const QString &text,
                 const QString &mergeKey = {});
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 9201; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(const Engine::Animation &a);
    Engine *m_e;
    quint64 m_id;
    Engine::Animation m_before, m_after;
    QString m_mergeKey;
};

// A timeline added at `index` (applied by redo); its id is kept across undo / redo
class AddAnimation : public QUndoCommand
{
public:
    AddAnimation(Engine *e, const Engine::Animation &a, int index, const QString &text);
    void undo() override;
    void redo() override;
    quint64 animationId() const { return m_a.id; }

private:
    Engine *m_e;
    Engine::Animation m_a;
    int m_index;
};

// A timeline deleted (applied by redo); undo puts it back with its id: the steps that drive it work again
class RemoveAnimation : public QUndoCommand
{
public:
    RemoveAnimation(Engine *e, int index);
    void undo() override { m_e->addAnimation(m_a, m_index); }
    void redo() override;

private:
    Engine *m_e;
    Engine::Animation m_a;
    int m_index;
};

// One animation of a layer's number (the Anim tab), by the layer's id and the number: before / after, either of them
// absent for one added or removed (it goes back to its place). The others are left alone (an edit made meanwhile by
// OSC is not undone with it). Edits with the same non-empty merge key that follow one another (a key dragged, the
// arrows of a number) are one step.
class SetLayerAnim : public QUndoCommand
{
public:
    SetLayerAnim(Engine *e, quint64 layer, const QString &param, std::optional<Animation> before, std::optional<Animation> after,
                 const QString &text, const QString &mergeKey = {});
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 9202; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(const std::optional<Animation> &a);
    Engine *m_e;
    quint64 m_layer;
    QString m_param;
    std::optional<Animation> m_before, m_after;
    int m_index; // its place when it was there
    QString m_mergeKey;
};

} // namespace cmd
