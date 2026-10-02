#pragma once
#include <QWidget>
#include <vector>

class QMenu;
class QTableWidget;
class QToolButton;

// Layer list, full width at the bottom of the window.
// The layer at the top of the list is drawn above the others. Audio layers have no picture: no opacity or blend.
class LayerTable : public QWidget
{
    Q_OBJECT
public:
    struct Row {
        QString name, tag, source, effects, blend, playback;
        bool visible = true, error = false;
        bool noPicture = false; // audio layer: no opacity or blend
        float opacity = 1.f;
    };

    explicit LayerTable(QWidget *parent = nullptr);

    void setRows(const std::vector<Row> &rows); // rebuilds if the count changes, otherwise updates in place
    int currentRow() const;
    void setCurrentRow(int row);
    QMenu *addMenu() const { return m_addMenu; }
    QWidget *table() const;

signals:
    void currentRowChanged(int row);
    void visibilityToggled(int row, bool visible);
    void opacityEdited(int row, double opacity);
    void removeClicked();
    void duplicateClicked();
    void moveClicked(int delta);

private:
    void updateRow(int r, const Row &row);
    QTableWidget *m_table;
    QMenu *m_addMenu;
    bool m_updating = false;
};
