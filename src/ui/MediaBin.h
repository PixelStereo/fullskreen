#pragma once
#include <QHash>
#include <QWidget>

class Engine;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;

// Media Bin: the project's video, image and audio files (used by layers or imported), grouped by type,
// and the ISF generators of the library (ISF > Generators). Items are dragged onto layers to load them.
class MediaBin : public QWidget
{
    Q_OBJECT
public:
    explicit MediaBin(Engine *engine, QWidget *parent = nullptr);

    void refresh();          // re-reads usage from the engine (called after every change)
    void importFiles(const QStringList &paths);
    QString selectedPath() const;

signals:
    void relinkRequested(const QString &from, const QString &to);
    void useAsSourceRequested(const QString &path); // load into the selected layer
    void binEdited(); // import / removal: the project is modified

private:
    void probe(const QString &path);
    void refreshIsf();
    void updateButtons();
    void importDialog();
    void relinkSelected();
    void removeSelected();
    void revealSelected();
    void contextMenu(const QPoint &pos);

    Engine *m_engine;
    QTreeWidget *m_tree;
    QTreeWidgetItem *m_videos, *m_images, *m_audios, *m_isf, *m_isfGenerators;
    QPushButton *m_relink, *m_remove, *m_reveal;
    QLabel *m_summary;
    QHash<QString, QString> m_info;  // path -> description (resolution, duration…)
    QHash<QString, bool> m_probing;
};
