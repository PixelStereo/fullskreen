#pragma once
// The editor of an animation (Anim.h), shared by the timelines of the show (TimelineEditor, in the Timelines window)
// and by the animations of a layer's numbers (ParamAnimEditor, in the Anim tab of the inspector and in their own
// floating window): the transport, the duration, the speed and the loop; a ruler and the time zoom; and one row per
// track — its settings (keys or a wave) and its lane, where the curve is drawn and edited.
//
// AnimEditor holds what is common. A subclass says where the animation lives (fetch, store, control) and what a
// track's settings show besides the common ones (makeRow: a TrackRow subclass).

#include "Engine.h"

#include <QWidget>
#include <functional>
#include <limits>
#include <map>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollBar;
class QSlider;
class QUndoStack;
class QVBoxLayout;
class NumberBox;
class IntBox;
class AnimEditor;

class QMenu;

// The name of a wave, of a key's curve
QString animWaveName(AnimWave w);
QString animCurveName(int curve);
// Numbers by category in a menu: "Spatial › Rotation" is "Rotation" in the sub-menu "Spatial". `item` adds the entry of
// each number (into: its menu; text: its name there).
void addParamMenu(QMenu *menu, const std::vector<Engine::AnimParam> &params,
                  const std::function<void(QMenu *into, const Engine::AnimParam &p, const QString &text)> &item);

// One track's curve (or its oscillator's wave) over the duration. A click adds a key, a key is dragged; dragging over an
// empty place draws: the keys under the stroke are replaced by the drawn ones. Double-click on a key: deleted;
// right-click: its curve towards the next key (an easing, Bézier, hold), its value, its time, delete. A Bézier curve
// shows its handles: dragged, they keep the key's tangent smooth (Alt: each on its own).
class CurveLane : public QWidget
{
    Q_OBJECT
public:
    explicit CurveLane(QWidget *parent = nullptr);
    // The track, the duration and the range of its number (widened to the keys)
    void setTrack(const AnimTrack &t, double duration, double lo, double hi);
    // What is shown: seconds t0..t1, values vlo..vhi (the zoom)
    void setView(double t0, double t1, double vlo, double vhi);
    void setPlayhead(double position, bool shown);
    void setCurrentValue(std::function<double()> f) { m_current = std::move(f); }
    void setLive(double v); // the value a "current value" first key starts from (captured, or the number now)
    double fullLo() const { return m_lo; }
    double fullHi() const { return m_hi; }
    const AnimTrack &track() const { return m_track; }
    static constexpr int kMargin = 10; // left and right: the ruler uses the same
    QSize sizeHint() const override { return QSize(500, 96); }

signals:
    // gesture: the edits of one drag share a number (one undo step); 0: an edit on its own
    void edited(const AnimTrack &t, int gesture);
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
    AnimTrack shown() const;           // the track as drawn (a "current value" key at the live value)
    int keyAt(QPointF p) const;
    // A Bézier handle under p: its key, and which side (true: out, towards the next key)
    bool handleAt(QPointF p, int *key, bool *out) const;
    void dragHandle(QPointF pos, bool alone);
    void emitEdited(int gesture = 0);
    AnimTrack m_track;
    double m_duration = 4, m_lo = 0, m_hi = 1;    // the whole
    double m_t0 = 0, m_t1 = 4, m_vlo = 0, m_vhi = 1; // what is shown
    double m_playhead = 0;
    double m_live = std::numeric_limits<double>::quiet_NaN();
    bool m_showPlayhead = false;
    int m_dragKey = -1;
    int m_dragHandle = -1; // the key whose handle is dragged
    bool m_dragOut = false;
    int m_gesture = 0;
    QPointF m_press;         // where a key drag started
    AnimKey m_pressKey;
    bool m_drawing = false;
    bool m_panning = false;
    QPointF m_panFrom;
    double m_lastDrawX = 0;
    std::vector<AnimKey> m_base, m_stroke; // drawing: the keys before, and the ones drawn
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

// A track: its settings (keys or a wave, and the wave's period, phase, center, amplitude) and its lane. A subclass
// adds its own settings above them (buildHeader) and fills them (fill).
class TrackRow : public QWidget
{
public:
    TrackRow(AnimEditor *editor, int index);
    void build(); // the widgets: called once, right after construction (buildHeader is virtual)
    void setPlayhead(double pos, bool shown);
    void setLive(double captured, bool running); // what a "current value" first key starts from
    void applyView();                            // the editor's zoom
    std::pair<double, double> fullRange() const;
    virtual void fill(); // the widgets from the track

protected:
    virtual void buildHeader(QGridLayout *) {} // above the common settings
    AnimTrack &track();
    const AnimTrack &track() const;
    void updateLane();
    void changed(bool lane = true, const QString &mergeKey = {}); // the track edited: to the engine
    std::pair<double, double> range() const;
    double currentValue() const;
    AnimEditor *m_ed;
    Engine *m_e;
    int m_i;
    bool m_filling = false;

private:
    QComboBox *m_kind = nullptr, *m_wave = nullptr;
    QWidget *m_osc = nullptr;
    NumberBox *m_period = nullptr, *m_center = nullptr, *m_amp = nullptr, *m_phase = nullptr;
    QPushButton *m_reseed = nullptr;
    class CurveLane *m_lane = nullptr;
};

class AnimEditor : public QWidget
{
    Q_OBJECT
public:
    // Side: a track's settings left of its lane (a wide window); Stacked: above it (the narrow inspector)
    enum class Layout { Side, Stacked };
    AnimEditor(Engine *engine, QUndoStack *undo, Layout layout, bool scrollTracks, QWidget *parent = nullptr);
    // m_edit from the engine (unless it is the same), the rows rebuilt. Call it once the subclass is built.
    void reload();
    void poll(); // where it is: the playhead, the time, the buttons
    bool isCommitting() const { return m_committing; } // its own edit is going to the engine
    Engine *engine() const { return m_engine; }
    Layout layout() const { return m_layout; }
    int headerWidth() const { return m_layout == Layout::Side ? kHeader : 0; }
    // The numbers a track can drive on its layer, and the range of its number
    const std::vector<Engine::AnimParam> &paramsOf(quint64 layer);
    std::pair<double, double> rangeOf(const AnimTrack &t);
    void setLaneHeight(int px) { m_laneHeight = px; }
    int laneHeight() const { return m_laneHeight; }

signals:
    void edited(); // changed: the project is modified

protected:
    friend class TrackRow;
    static constexpr int kHeader = 300; // width of a track's settings, left of its lane (Side)
    // Where the animation lives
    virtual bool fetch(Animation *a) const = 0; // as the engine has it now (false: none)
    // m_edit replacing `before`, as an undo step. Edits with the same non-empty key that follow one another (a drag,
    // the arrows of a number) form one step.
    virtual void store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey) = 0;
    virtual void control(AnimAction action, double time = 0) = 0;
    virtual TrackRow *makeRow(int index) { return new TrackRow(this, index); }
    virtual void loaded() {} // after a reload (the subclass's own widgets)
    void commit(const QString &text, const QString &mergeKey = {});
    void rebuildRows();
    void forgetView(); // another animation: shown whole on the next reload
    void forgetParams() { m_params.clear(); } // the layers changed: their numbers are read again
    void setEditorEnabled(bool on);
    QVBoxLayout *mainLayout() const { return m_main; }
    struct LaneView {
        double zoom = 1;                                          // values: the whole range / zoom
        double center = std::numeric_limits<double>::quiet_NaN(); // nan: the middle of the range
    };
    LaneView &laneView(int track);
    void viewOfLane(int track, double lo, double hi, double *vlo, double *vhi);
    void applyView(); // the time window and the lanes' value windows to the ruler, the scroll bar, the lanes
    void zoomTime(double factor, double anchor);
    void panTime(double seconds);
    void setTimeZoom(double factor); // around the middle of what is shown
    void fitView();   // the time from the first key to the last, each lane from its lowest value to its highest
    void resetView();

    Engine *m_engine;
    QUndoStack *m_undo;
    Layout m_layout;
    Animation m_edit;
    bool m_has = false; // there is an animation to edit
    bool m_filling = false, m_committing = false;
    double m_t0 = 0, m_t1 = 4; // seconds shown
    std::vector<LaneView> m_laneViews;
    std::vector<TrackRow *> m_rows;
    QPushButton *m_play, *m_pause, *m_stop, *m_rewind, *m_fit, *m_all;
    QSlider *m_zoomX = nullptr, *m_zoomY = nullptr;
    QScrollBar *m_scrollX;
    QLabel *m_time;
    NumberBox *m_duration, *m_speed;
    QComboBox *m_loop;
    IntBox *m_repeat;
    TimeRuler *m_ruler;
    QWidget *m_body;          // all but what a subclass adds around it
    QVBoxLayout *m_tracks;    // the rows (then a stretch when they scroll)
    bool m_scrollTracks;

private:
    QVBoxLayout *m_main;
    std::map<quint64, std::vector<Engine::AnimParam>> m_params; // by layer, until the next reload
    bool m_freshView = true;
    int m_laneHeight = 96;
};

// Is `a` the same animation as `b`, as edited (its transport where it is left out)?
bool sameAnimation(const Animation &a, const Animation &b);
