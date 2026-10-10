#pragma once
#include "AnimEditor.h"

#include <QListWidget>
#include <QWidget>

class QPushButton;
class QUndoStack;

// A timeline dragged from the list of the timelines carries its id (onto a step of a sequence)
inline constexpr const char *kTimelineMime = "application/x-fulskrin-timeline";

// The list of the timelines, a drag source
class TimelineList : public QListWidget
{
    Q_OBJECT
public:
    using QListWidget::QListWidget;

protected:
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
};

// A timeline of the show (Engine::animation), edited: its tracks each drive a number of a layer (or of the
// composition), chosen in the track's settings; tracks are added for the layer selected in the layer list
class TimelineEditor : public AnimEditor
{
    Q_OBJECT
public:
    TimelineEditor(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);
    void setTimeline(quint64 id); // the one shown (0: none)
    quint64 timeline() const { return m_id; }
    void setCurrentLayer(quint64 id) { m_currentLayer = id; } // the layer a new track drives
    void deleteTrack(int index);

protected:
    bool fetch(Animation *a) const override;
    void store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey) override;
    void control(AnimAction action, double time) override;
    TrackRow *makeRow(int index) override;

private:
    friend class TimelineTrackRow;
    quint64 m_id = 0, m_currentLayer = 0;
    QPushButton *m_addTrack;
};

// The timelines of the show, in a floating window that stays in front: the list (new, duplicate, delete, rename;
// a timeline dragged onto a step of a sequence), and the one selected in a TimelineEditor.
class TimelineWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineWindow(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);
    void setCurrentLayer(quint64 id) { m_editor->setCurrentLayer(id); } // the layer a new track drives

signals:
    void edited(); // the timelines changed: the project is modified

private:
    void refreshList();
    void showCurrent();
    void pollList(); // the ones playing
    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    TimelineList *m_list;
    QPushButton *m_add, *m_dup, *m_del;
    TimelineEditor *m_editor;
    quint64 m_current = 0;
    bool m_filling = false, m_renaming = false;
};
