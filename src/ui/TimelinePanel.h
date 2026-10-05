#pragma once
#include "Engine.h"
#include <QListWidget>
#include <QWidget>
#include <functional>
#include <limits>
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
    // The track, the timeline's duration and the range of its number (widened to the keys)
    void setTrack(const Engine::AnimTrack &t, double duration, double lo, double hi);
    // What is shown: seconds t0..t1, values vlo..vhi (the zoom)
    void setView(double t0, double t1, double vlo, double vhi);
    void setPlayhead(double position, bool shown);
    void setCurrentValue(std::function<double()> f) { m_current = std::move(f); }
    void setLive(double v); // the value a "current value" first key starts from (captured, or the number now)
    double fullLo() const { return m_lo; }
    double fullHi() const { return m_hi; }
    const Engine::AnimTrack &track() const { return m_track; }
    static constexpr int kMargin = 10; // left and right: the ruler uses the same
    QSize sizeHint() const override { return QSize(500, 96); }

signals:
    // gesture: the edits of one drag share a number (one undo step); 0: an edit on its own
    void edited(const Engine::AnimTrack &t, int gesture);
    void zoomTime(double factor, double anchor); // ⌘/Ctrl + wheel
    void panTime(double seconds);                // Shift + wheel, horizontal wheel, middle drag
    void zoomValue(double factor, double anchor); // Alt + wheel
    void panValue(double delta);                  // middle drag

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    double xOf(double t) const;
    double yOf(double v) const;
    double tOf(double x) const;
    double vOf(double y) const;
    double shownValue(size_t k) const; // a key's value as drawn
    int keyAt(QPointF p) const;
    void emitEdited(int gesture = 0);
    Engine::AnimTrack m_track;
    double m_duration = 4, m_lo = 0, m_hi = 1;    // the whole
    double m_t0 = 0, m_t1 = 4, m_vlo = 0, m_vhi = 1; // what is shown
    double m_playhead = 0;
    double m_live = std::numeric_limits<double>::quiet_NaN();
    bool m_showPlayhead = false;
    int m_dragKey = -1;
    int m_gesture = 0;
    QPointF m_press;         // where a key drag started
    Engine::AnimKey m_pressKey;
    bool m_drawing = false;
    bool m_panning = false;
    QPointF m_panFrom;
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
    void setView(double t0, double t1);
    void setPlayhead(double position, bool shown);
    QSize sizeHint() const override { return QSize(500, 22); }

signals:
    void seekRequested(double t);
    void zoomTime(double factor, double anchor);
    void panTime(double seconds);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    double tOf(double x) const;
    double m_duration = 4, m_t0 = 0, m_t1 = 4, m_playhead = 0;
    bool m_shown = false;
};

// The timelines of the show, in a floating window that stays in front: the list (new, duplicate, delete, rename;
// a timeline dragged onto a step of a sequence), and the one selected: its transport (play, pause, stop, rewind,
// the time), duration and loop mode, and its tracks — each one a number of a layer (or of the composition), drawn as
// a curve or given by an oscillator.
class QUndoStack;
class QSlider;
class QScrollBar;

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
    struct LaneView {
        double zoom = 1;                                          // values: the whole range / zoom
        double center = std::numeric_limits<double>::quiet_NaN(); // nan: the middle of the range
    };
    void refreshList();
    void loadEditor();  // the selected timeline's settings and tracks
    void rebuildRows();
    // m_edit to the engine, as an undo step. Edits with the same non-empty key that follow one another
    // (a drag, the arrows of a number) form one step.
    void commit(const QString &text = QStringLiteral("Edit Timeline"), const QString &mergeKey = {});
    void poll();        // where it is
    // Zoom
    void applyView();   // the time window and the lanes' value windows to the ruler, the scroll bar, the lanes
    void zoomTime(double factor, double anchor);
    void panTime(double seconds);
    void setTimeZoom(double factor); // around the middle of what is shown
    LaneView &laneView(int track);
    void viewOfLane(int track, double lo, double hi, double *vlo, double *vhi);
    void fitView();     // the time from the first key to the last, each lane from its lowest value to its highest
    void resetView();
    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    TimelineList *m_list;
    QPushButton *m_add, *m_dup, *m_del;
    QWidget *m_editor;
    QPushButton *m_play, *m_pause, *m_stop, *m_rewind, *m_addTrack;
    QPushButton *m_fit, *m_all;
    QSlider *m_zoomX, *m_zoomY;
    QScrollBar *m_scrollX;
    QLabel *m_time;
    NumberBox *m_duration;
    NumberBox *m_speed;
    QComboBox *m_loop;
    IntBox *m_repeat;
    TimeRuler *m_ruler;
    QWidget *m_tracksHost;
    QVBoxLayout *m_tracks;
    std::vector<TrackRow *> m_rows;
    quint64 m_current = 0, m_currentLayer = 0, m_viewOf = 0;
    Engine::Animation m_edit;
    double m_t0 = 0, m_t1 = 4; // seconds shown
    std::vector<LaneView> m_laneViews;
    bool m_filling = false, m_committing = false;
};
