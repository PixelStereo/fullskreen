#pragma once
#include <QWidget>
#include <functional>
#include <vector>

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

    // Shows the current values (a memory fading them, OSC), except in the field being edited
    void refresh();

signals:
    void rebuildRequested();

private:
    void setValue(int input, const QString &label, const std::function<void(IsfValue &)> &modify);

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer, m_slot;
    std::vector<std::pair<int, std::function<void(const IsfValue &)>>> m_followers; // input, shows its value
};
