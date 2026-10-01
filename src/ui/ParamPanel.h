#pragma once
#include <QWidget>
#include <functional>

class Engine;
class QUndoStack;
struct IsfValue;

// Panneau de paramètres généré automatiquement à partir des INPUTS d'un shader ISF.
// slot -1 = générateur du calque, sinon index de l'effet. Chaque modification passe par la pile d'annulation.
class ParamPanel : public QWidget
{
    Q_OBJECT
public:
    ParamPanel(Engine *engine, QUndoStack *undo, int layer, int slot, QWidget *parent = nullptr);

signals:
    void rebuildRequested();

private:
    void setValue(int input, const QString &label, const std::function<void(IsfValue &)> &modify);

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer, m_slot;
};
