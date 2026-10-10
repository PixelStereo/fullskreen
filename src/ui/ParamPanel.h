#pragma once
#include <QWidget>
#include <functional>
#include <vector>

class Engine;
class QUndoStack;
class SliderField;
class AnimateMenu;
struct IsfValue;

// Parameter panel generated automatically from the INPUTS of an ISF shader.
// slot -1 = the layer's generator, otherwise the effect index. Every edit goes through the undo stack.
class ParamPanel : public QWidget
{
    Q_OBJECT
public:
    ParamPanel(Engine *engine, QUndoStack *undo, int layer, int slot, QWidget *parent = nullptr);

    // Shows the current values (a snapshot fading them, OSC), except in the field being edited
    void refresh();
    // A right-click on a parameter offers to animate it (its numbers, and the shader's speed)
    void attachAnimate(AnimateMenu *menu);

signals:
    void rebuildRequested();

private:
    void setValue(int input, const QString &label, const std::function<void(IsfValue &)> &modify);

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer, m_slot;
    SliderField *m_speed = nullptr;
    std::vector<std::pair<int, std::function<void(const IsfValue &)>>> m_followers; // input, shows its value
    // The widgets of each number (its name, its field; a point's x and y fields), for the Animate menu
    struct Row {
        QString input;              // the input's name ("" : the speed)
        std::vector<QWidget *> all; // the name and the whole field: every number of the input
        QWidget *x = nullptr, *y = nullptr;
    };
    std::vector<Row> m_rows;
};
