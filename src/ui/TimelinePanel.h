#pragma once
#include "Engine.h"
#include <QListWidget>
#include <QWidget>
#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class NumberBox;
class IntBox;
class TimeRuler;
class TrackRow;

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

// One track's curve (or its oscillator's wave) over the timeline's duration. A click adds a key, a key is dragged;
// dragging over an empty place draws: the keys under the stroke are replaced by the drawn ones. Double-click on a
// key: deleted; right-click: its curve towards the next key, its value, delete.
class CurveLane : public QWidget
{
    Q_OBJECT
public:
    explicit CurveLane(QWidget *parent = nullptr);
    void setTrack(const Engine::AnimTrack &t, double duration, double lo, double hi);
    void setPlayhead(double position, bool shown);
    void setCurrentValue(std::function<double()> f) { m_current = std::move(f); }
    static constexpr int kMargin = 10; // left and right: the ruler uses the same
    QSize sizeHint() const override { return QSize(500, 96); }

signals:
    void edited(const Engine::AnimTrack &t);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    double xOf(double t) const;
    double yOf(double v) const;
    double tOf(double x) const;
    double vOf(double y) const;
    int keyAt(QPointF p) const;
    void emitEdited();
    Engine::AnimTrack m_track;
    double m_duration = 4, m_lo = 0, m_hi = 1;
    double m_playhead = 0;
    bool m_showPlayhead = false;
    int m_dragKey = -1;
    bool m_drawing = false;
    double m_lastDrawX = 0;
    std::vector<Engine::AnimKey> m_base, m_stroke; // drawing: the keys before, and the ones drawn
    std::function<double()> m_current;
};

// The seconds over the lanes; a click or a drag seeks (the values follow at once)
class TimeRuler : public QWidget
{
    Q_OBJECT
public:
    explicit TimeRuler(QWidget *parent = nullptr);
    void setDuration(double d);
    void setPlayhead(double position, bool shown);
    QSize sizeHint() const override { return QSize(500, 22); }

signals:
    void seekRequested(double t);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    double m_duration = 4, m_playhead = 0;
    bool m_shown = false;
};

// The timelines of the show, in a floating window that stays in front: the list (new, duplicate, delete, rename;
// a timeline dragged onto a step of a sequence), and the one selected: its transport (play, pause, stop, rewind,
// the time), duration and loop mode, and its tracks — each one a number of a layer (or of the composition), drawn as
// a curve or given by an oscillator.
class QUndoStack;

class TimelineWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineWindow(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);
    void setCurrentLayer(quint64 id) { m_currentLayer = id; } // the layer a new track drives

signals:
    void edited(); // the timelines changed: the project is modified

private:
    friend class TrackRow;
    void refreshList();
    void loadEditor();  // the selected timeline's settings and tracks
    void rebuildRows();
    void commit();      // m_edit to the engine
    void poll();        // where it is
    void fitDuration(); // adjust duration to show all keys
    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    TimelineList *m_list;
    QPushButton *m_add, *m_dup, *m_del;
    QWidget *m_editor;
    QPushButton *m_play, *m_pause, *m_stop, *m_rewind, *m_addTrack;
    QPushButton *m_fit;
    QLabel *m_time;
    NumberBox *m_duration;
    NumberBox *m_speed;
    QComboBox *m_loop;
    IntBox *m_repeat;
    TimeRuler *m_ruler;
    QWidget *m_tracksHost;
    QVBoxLayout *m_tracks;
    std::vector<TrackRow *> m_rows;
    quint64 m_current = 0, m_currentLayer = 0;
    Engine::Animation m_edit;
    bool m_filling = false, m_committing = false;
};
