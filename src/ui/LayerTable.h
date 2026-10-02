#pragma once
#include <QWidget>
#include <vector>

class QMenu;
class QTableWidget;
class QToolButton;

// Liste des calques, en bas de la fenêtre sur toute la largeur.
// Le calque du haut de la liste est affiché au-dessus des autres.
class LayerTable : public QWidget
{
    Q_OBJECT
public:
    struct Row {
        QString name, tag, source, effects, blend, playback;
        bool visible = true, error = false;
        float opacity = 1.f;
    };

    explicit LayerTable(QWidget *parent = nullptr);

    void setRows(const std::vector<Row> &rows); // reconstruit si le nombre change, sinon met à jour sur place
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
