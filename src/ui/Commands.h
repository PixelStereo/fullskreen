#pragma once
// Commandes d'annulation (QUndoStack). Chaque commande s'applique au moteur sous son verrou.
// Les calques sont repérés par leur index : la pile garantit que l'ordre des index est cohérent
// au moment où une commande est annulée ou rejouée.

#include "Engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoCommand>
#include <QVariant>

namespace cmd {

enum Id { ParamId = 1, PropId = 2, NudgeId = 3 };

// Instance ISF d'un calque : slot -1 = générateur, sinon index de l'effet. Verrou du moteur requis.
IsfInstance *resolveIsf(Engine *e, int layer, int slot);

// Paramètre d'un shader ISF (fusionne les mouvements continus d'un même curseur)
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

// Propriété simple d'un calque
class SetLayerProp : public QUndoCommand
{
public:
    enum Prop { Name, Visible, Opacity, Blend, Speed, Loop };
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

// Mapping complet (coins + grille) avant / après une modification
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

// Calque ajouté (déjà fait au moment du push)
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

// Calque supprimé
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

// Calque remplacé par un autre état complet (changement de source…) ; déjà fait au moment du push
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

// Chaîne d'effets avant / après (ajout, suppression, ordre, activation) ; déjà fait au moment du push
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
