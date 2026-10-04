#pragma once
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <vector>

class Engine;
class QUndoStack;
class QLabel;
class QLineEdit;
class QDoubleSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QVBoxLayout;

// Drag and drop of a memory (onto a step of a sequence): its id, as text
inline constexpr const char *kMemoryMime = "application/x-fulskrin-memory";

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
    QString label;            // as listed
    QString timeKey;          // a value that fades: the key of its time in the memory (empty otherwise)
};

// Memories (as the scenes / cues of MadMapper), at the bottom of the window, in three parts:
//  - the list of the memories (number, name, fade): + stores the current state, double-click / Enter / GO recalls
//    (with the memory's fade; undoable); a memory is dragged from there onto a step of a sequence;
//  - what the selected memory holds: its picture, name and fade, then its layers, each unfolding into its values
//    (source, roi, color, mapping, effects); a layer unchecked is left alone by the recall;
//  - the value selected there: its stored value, editable (the memory changes, the composition does not), and
//    for a value that fades, how it gets there: CUT (at once), FOLLOW (the memory's fade) or seconds of its own.
class MemoryPanel : public QWidget
{
    Q_OBJECT
public:
    MemoryPanel(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);
    void refresh(); // memories changed (project opened…)
    void recall(int i);
    int activeMemory() const { return m_active; } // last recalled (-1: none)

signals:
    void edited();   // memories changed: the project is modified
    void recalled(); // the layers changed

private:
    void store();
    void updateMemory(int i); // stores the current state into it (layers left out stay out, times are kept)
    void removeMemory(int i);
    void showInspector(int i);
    void showDetail(QTreeWidgetItem *it); // the selected value, on the right
    void fillLayer(QTreeWidgetItem *parent, int row, const QJsonObject &layer);
    QTreeWidgetItem *addField(QTreeWidgetItem *parent, const MemField &f, const QJsonValue &value);
    void applyField(const MemField &f, const QJsonValue &value); // writes it into the selected memory
    void applyTime(int row, const QString &key, double seconds); // < 0: FOLLOW (the memory's fade)
    void setInclusion(int i, quint64 layerId, bool included);
    void refreshRows(); // value and time texts of the tree, after an edit (no rebuild)
    int selected() const;

    Engine *m_engine;
    QUndoStack *m_undo;
    QTreeWidget *m_list;
    QLabel *m_thumb, *m_title;
    QLineEdit *m_name;
    QDoubleSpinBox *m_fade;
    QTreeWidget *m_layers;
    QPushButton *m_go, *m_update, *m_delete, *m_store;
    QWidget *m_inspector, *m_detail;
    QVBoxLayout *m_detailLayout;
    std::vector<MemField> m_fields;
    QSet<QString> m_expanded; // unfolded nodes (layer id / section), kept across refreshes
    QString m_selectedKey;    // value selected in the tree, kept across refreshes
    int m_active = -1;        // last recalled
    bool m_filling = false, m_applying = false;
};
