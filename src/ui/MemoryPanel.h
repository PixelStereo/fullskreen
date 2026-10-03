#pragma once
#include <QWidget>

class Engine;
class QUndoStack;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QLineEdit;
class QDoubleSpinBox;
class QTreeWidget;
class QPushButton;

// Memories (as the scenes / cues of MadMapper), at the bottom of the window: a grid of thumbnails ending with "+",
// which stores the current state of the layers, and an inspector showing what the selected memory contains.
// Click: select (inspector) · double-click, Enter or GO: recall (with the memory's fade; undoable).
// In the inspector, a layer can be left out of the memory (unchecked): the recall does not touch it.
class MemoryPanel : public QWidget
{
    Q_OBJECT
public:
    MemoryPanel(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);
    void refresh(); // memories changed (project opened…)

signals:
    void edited();   // memories changed: the project is modified
    void recalled(); // the layers changed

private:
    void store();
    void recall(int i);
    void updateMemory(int i); // stores the current state into it (layers left out stay out)
    void removeMemory(int i);
    void showInspector(int i);
    void setInclusion(int i, quint64 layerId, bool included);
    int selected() const;

    Engine *m_engine;
    QUndoStack *m_undo;
    QListWidget *m_grid;
    QLabel *m_thumb, *m_title;
    QLineEdit *m_name;
    QDoubleSpinBox *m_fade;
    QTreeWidget *m_layers;
    QPushButton *m_go, *m_update, *m_delete;
    QWidget *m_inspector;
    int m_active = -1; // last recalled
    bool m_filling = false;
};
