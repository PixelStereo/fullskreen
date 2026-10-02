#pragma once
#include <QWidget>
#include <functional>

class Engine;
class QUndoStack;
struct IsfValue;

// Parameter panel generated automatically from the INPUTS of an ISF shader.
// slot -1 = the layer's generator, otherwise the effect index. Every edit goes through the undo stack.
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
