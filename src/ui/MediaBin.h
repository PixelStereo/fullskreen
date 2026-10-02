#pragma once
#include <QHash>
#include <QWidget>

class Engine;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;

// Chutier : tous les fichiers images / vidéos externes du projet, classés par type.
// Fichiers utilisés par les calques (sources, images des shaders) et fichiers importés.
class MediaBin : public QWidget
{
    Q_OBJECT
public:
    explicit MediaBin(Engine *engine, QWidget *parent = nullptr);

    void refresh();          // relit l'usage dans le moteur (appelé après chaque modification)
    void importFiles(const QStringList &paths);
    QString selectedPath() const;

signals:
    void relinkRequested(const QString &from, const QString &to);
    void useAsSourceRequested(const QString &path);
    void newLayerRequested(const QString &path);
    void binEdited(); // import / retrait : le projet est modifié

private:
    void probe(const QString &path);
    void updateButtons();
    void importDialog();
    void relinkSelected();
    void removeSelected();
    void revealSelected();
    void contextMenu(const QPoint &pos);

    Engine *m_engine;
    QTreeWidget *m_tree;
    QTreeWidgetItem *m_videos, *m_images;
    QPushButton *m_relink, *m_remove, *m_reveal;
    QLabel *m_summary;
    QHash<QString, QString> m_info;  // chemin -> description (résolution, durée…)
    QHash<QString, bool> m_probing;
};
