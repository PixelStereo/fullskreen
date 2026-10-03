#pragma once
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <vector>

class Engine;
class QUndoStack;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QLineEdit;
class QDoubleSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

// One editable (or informative) value stored in a memory, found by its path inside the layer's JSON.
struct MemField {
    enum Kind { Info, Bool, Number, Choice };
    Kind kind = Info;
    int row = 0;          // index of the layer in the memory
    QStringList path;     // inside the layer object, e.g. {"source", "speed"} or {"effects", "0", "params", "radius"}
    double min = 0, max = 1, step = 0.01, scale = 1; // scale: displayed value = stored × scale (percentages)
    int decimals = 2;
    QString suffix;
    QStringList keys, labels; // Choice: stored key ↔ shown label
};

// Memories (as the scenes / cues of MadMapper), at the bottom of the window: a grid of thumbnails ending with "+",
// which stores the current state of the layers, and an inspector showing what the selected memory contains.
// Click: select (inspector) · double-click, Enter or GO: recall (with the memory's fade; undoable).
// In the inspector, a layer can be left out of the memory (unchecked): the recall does not touch it.
// Each layer unfolds (source, roi, color, mapping, effects): every stored value can be read and edited there,
// which changes what the memory will apply, without touching the composition. Each value that fades has a Time:
// the memory's fade (Transition), a cut, or a time of its own.
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
    void fillLayer(QTreeWidgetItem *parent, int row, const QJsonObject &layer);
    QTreeWidgetItem *addField(QTreeWidgetItem *parent, const QString &label, const MemField &f, const QJsonValue &value);
    void applyField(const MemField &f, const QJsonValue &value); // writes it into the selected memory
    void applyTime(int row, const QString &key, double seconds); // < 0: the memory's fade
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
    std::vector<MemField> m_fields;
    QSet<QString> m_expanded; // unfolded nodes (layer id / section), kept across refreshes
    int m_active = -1; // last recalled
    bool m_filling = false, m_applying = false;
};
