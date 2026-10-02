#pragma once
#include <QWidget>
#include <vector>

class QTableWidget;
class QToolButton;

// Layer list, full width at the bottom of the window.
// The layer at the top of the list is drawn above the others. Audio layers have no picture: no opacity or blend.
// "+" creates an empty layer; media dropped on a row (from the Media Bin or the Finder) is loaded into that layer.
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
    QWidget *table() const;

signals:
    void currentRowChanged(int row);
    void addClicked();
    void filesDropped(int row, const QStringList &paths); // row -1: below the last layer
    void visibilityToggled(int row, bool visible);
    void opacityEdited(int row, double opacity);
    void removeClicked();
    void duplicateClicked();
    void moveClicked(int delta);

private:
    void updateRow(int r, const Row &row);
    QTableWidget *m_table;
    bool m_updating = false;
};
