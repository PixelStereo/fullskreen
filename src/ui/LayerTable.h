#pragma once
#include <QList>
#include <QWidget>
#include <vector>

class QTableWidget;
class QToolButton;

// Layer list, full width at the bottom of the window.
// The layer at the top of the list is drawn above the others. Audio layers have no picture: no opacity or blend.
// "+" creates an empty layer; media dropped on a row (from the Media Bin or the Finder) is loaded into that layer.
// Groups fold and unfold (arrow), layers are dragged onto a group to go into it, or between rows to move.
// Double-click (or F2) on a name renames it; the padlock between visibility and name locks the layer.
// Rows follow the engine's layer indices one to one (members of a folded group are hidden rows).
// A drag of layers from the list: their rows, "3,5"
inline constexpr char kLayerRowsMime[] = "application/x-fulskrin-layer-rows";

class LayerTable : public QWidget
{
    Q_OBJECT
public:
    struct Row {
        QString name, tag, source, effects, blend, playback;
        bool enabled = true, error = false;
        bool noPicture = false; // audio layer: no opacity or blend
        float opacity = 1.f;
        bool viewport = false;      // a viewport: in the block at the top of the list
        bool group = false, collapsed = false; // collapsed: the group itself is folded
        bool hidden = false;        // inside a folded group
        int depth = 0;              // 0 at the top level, 1 in a group, 2 in a group in a group…
        bool locked = false, lockedByGroup = false, effectsOn = true;
        int effectCount = 0;
    };

    explicit LayerTable(QWidget *parent = nullptr);

    void setRows(const std::vector<Row> &rows); // rebuilds if the count changes, otherwise updates in place
    int currentRow() const;
    void setCurrentRow(int row);
    QList<int> selectedRows() const; // in order
    void startRename(int row);
    QWidget *table() const;

signals:
    void currentRowChanged(int row);
    void addClicked();
    void groupClicked();
    void viewportClicked();
    void filesDropped(int row, const QStringList &paths); // row -1: below the last layer
    void visibilityToggled(int row, bool visible);
    void lockToggled(int row);
    void collapseToggled(int row);
    void renamed(int row, const QString &name);
    void opacityEdited(int row, double opacity);
    // Rows dragged: inserted before `beforeRow` (-1: at the end), into the group `parentRow` (-1: top level)
    void moveRequested(const QList<int> &rows, int beforeRow, int parentRow);
    void removeClicked();
    void duplicateClicked();
    void moveClicked(int delta);
    void contextMenuRequested(int row, const QPoint &globalPos); // right-click on a row (-1: empty area)

private:
    void updateRow(int r, const Row &row);
    void restyleSelection();
    QTableWidget *m_table;
    std::vector<Row> m_rows;
    bool m_updating = false;
};
