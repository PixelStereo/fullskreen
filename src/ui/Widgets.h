#pragma once
// Small interface building blocks shared by the inspector and the parameter panels.

#include <QBrush>
#include <QCheckBox>
#include <QToolButton>
#include <QColor>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QObject>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QRectF>
#include <QWidget>
#include <functional>
#include <vector>

class QDoubleSpinBox;
class QSlider;
class QLineEdit;

// Accent color of the interface: the selection, the bars, the buttons that are on, the links.
// A very light grey by default, changed in the Settings tab and kept for this machine.
namespace theme {
class Notifier : public QObject
{
    Q_OBJECT
public:
signals:
    void changed(); // the accent changed: restyle and rebuild the panels
};
Notifier *notifier();
void applyToApplication(class QApplication &app); // palette and global stylesheet (called again on a change)
QColor accent();
QColor defaultAccent();
void setAccent(const QColor &c); // saved, then everything using it is restyled
QColor onAccent();               // text and icons drawn on the accent: dark or light, whichever reads
QString css(int alpha = 255);    // "#rrggbb", or "rgba(r, g, b, a)" when alpha < 255
} // namespace theme

// Magnetism: points and frames dragged in the preview are caught by the edges, centers and corners around
// them, and the bars by their notable values. On or off for the session (button next to the zoom); the state
// at start and the catching distance are settings.
namespace magnet {
class Notifier : public QObject
{
    Q_OBJECT
public:
signals:
    void changed();
};
Notifier *notifier();
bool enabled();
void setEnabled(bool on);
bool enabledAtStart(); // Settings (on unless changed)
void setEnabledAtStart(bool on);
int distance();        // screen pixels within which something is caught (Settings, 8 by default)
void setDistance(int px);
QIcon icon();          // a horseshoe magnet
} // namespace magnet

// Parameter name: a click puts the parameter back to its default value.
class ResetLabel : public QLabel
{
    Q_OBJECT
public:
    ResetLabel(const QString &text, std::function<void()> reset, QWidget *parent = nullptr);
    void setReset(std::function<void()> reset) { m_reset = std::move(reset); } // when it needs what is built after it

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    std::function<void()> m_reset;
};

// One number, one control: a bar that is dragged, graduated with tick marks, and the value shown and typed
// at its right end. Replaces the slider + spin box + separate value label of the earlier panels.
// A click on the bar jumps to that value, a drag follows the cursor, the wheel steps, and the field at the
// right takes a typed value (with its unit). Fine adjustment with Shift.
class SliderField : public QWidget
{
    Q_OBJECT
public:
    explicit SliderField(QWidget *parent = nullptr);
    void setRange(double min, double max);
    // Widens what the field accepts beyond the bar: the bar covers the useful range, extreme values are typed
    void setTypedRange(double min, double max);
    void setDecimals(int d);
    void setSuffix(const QString &s);  // " %", " s", " ×"
    void setSingleStep(double s);
    void setTicks(int n);              // tick marks drawn on the bar (0: none)
    void setSnaps(const std::vector<double> &v); // values the cursor catches while dragging (0, 100 %…)
    // Paints the bar with this gradient instead of a fill: the value is then shown as a marker on it
    void setGradient(const QGradientStops &stops);
    void setOrigin(double v);          // where the fill starts (0 by default; 0 in the middle for a ± range)
    void setValue(double v);           // no signal
    double value() const { return m_value; }
    bool isDragging() const { return m_drag; }
    QSize sizeHint() const override { return QSize(220, 24); }

signals:
    void valueEdited(double v);   // continuous while dragging, and on a typed value
    void editingFinished(double v);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    QRectF bar() const;
    double valueAt(double x) const;
    void apply(double v, bool finished);
    class QDoubleSpinBox *m_spin;
    double m_value = 0, m_min = 0, m_max = 1, m_origin = 0;
    int m_ticks = 0;
    bool m_drag = false;
    std::vector<double> m_snaps;
    QGradientStops m_gradient;
};

// Two numbers on one bar: the chosen range is highlighted between two handles, with its bounds typed at
// either end. Used for the in / out points of a video or a sound.
class RangeField : public QWidget
{
    Q_OBJECT
public:
    explicit RangeField(QWidget *parent = nullptr);
    void setRange(double min, double max); // the bounds of the bar
    void setDecimals(int d);
    void setSuffix(const QString &s);
    void setValues(double lo, double hi); // no signal
    double low() const { return m_lo; }
    double high() const { return m_hi; }
    bool isDragging() const { return m_drag != 0; }
    QSize sizeHint() const override { return QSize(260, 24); }

signals:
    void edited(bool low, double v); // continuous while dragging
    void editingFinished();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    QRectF bar() const;
    double valueAt(double x) const;
    double xOf(double v) const;
    class QDoubleSpinBox *m_loSpin, *m_hiSpin;
    double m_lo = 0, m_hi = 1, m_min = 0, m_max = 1;
    int m_drag = 0; // 0 none, 1 low, 2 high
};

// Transport and play-mode icons, drawn (no symbol font needed), as the padlock is.
enum class TransportIcon { PlayBack, Pause, PlayForward, ToStart, StepBack, StepForward, MarkIn, MarkOut };
QIcon transportIcon(TransportIcon kind);
QIcon playModeIcon(int mode); // PlayMode: OneShot, Loop, PingPong, Stop

// Part of the source picture used by a layer: preview of the whole picture with a rectangle whose sides are dragged
// (they stay straight); dragging inside moves the rectangle. Numeric fields in % below.
class RoiEditor : public QWidget
{
    Q_OBJECT
public:
    explicit RoiEditor(QWidget *parent = nullptr);
    void setImage(const QImage &img);
    void setAspect(double aspect); // used while no preview is available
    void setRoi(const QRectF &r);
    QRectF roi() const { return m_roi; }
    QSize sizeHint() const override { return QSize(320, 200); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

signals:
    void roiEdited(const QRectF &r); // continuous while dragging

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    QRectF picture() const; // image area in the widget
    int hit(QPointF p) const;   // 0 left, 1 top, 2 right, 3 bottom, 4 inside, -1 none
    QImage m_image;
    double m_aspect = 16.0 / 9.0;
    QRectF m_roi{0, 0, 1, 1}, m_dragStart;
    int m_drag = -1;
    QPointF m_pressNorm;
};

// One color in the chosen models: RGB (0..255, hex), HSL, additive (R G B light %), subtractive (C M Y %),
// or all of them; the models are linked. Each channel name resets it.
class ColorEditor : public QWidget
{
    Q_OBJECT
public:
    enum Model { Rgb = 1, Hsl = 2, Additive = 4, Subtractive = 8, All = 15 };
    ColorEditor(const QString &title, const QColor &defaultColor, QWidget *parent = nullptr);
    void setModels(int models);
    // Switch shown next to the title: the color is kept but not applied while it is off
    void setSwitch(bool shown, bool on, const QString &tip = QString());
    void setColor(const QColor &c);
    QColor color() const { return m_color; }

signals:
    void colorEdited(const QColor &c);
    void switchToggled(bool on);

private:
    struct Channel {
        int model;
        int index;
        SliderField *bar = nullptr;
        double max = 1;
        QWidget *row = nullptr;
    };
    void build(const QString &title);
    class QCheckBox *m_switch = nullptr;
    void addChannel(int model, int index, const QString &name, double max, const QString &suffix, const QString &gradient);
    double channelValue(int model, int index) const;
    void setChannel(int model, int index, double v);
    void sync(const Channel *except = nullptr);
    void apply(const QColor &c, bool emitSignal);

    QColor m_color, m_default;
    float m_h = 0, m_s = 0, m_l = 0; // kept so that hue does not jump on grays
    std::vector<Channel> m_channels;
    QWidget *m_groups[4] = {};
    QLabel *m_swatch = nullptr;
    QLineEdit *m_hex = nullptr;
    bool m_syncing = false;
};

// Padlock drawn by hand (no emoji font needed): closed and red when locked, closed and dim when locked by the group,
// open and faint otherwise.
QIcon padlockIcon(bool locked, bool inherited = false);

// Popup list with a search field (Add Effect…): items filed by group, filtered while typing,
// Enter or a click picks one.
class SearchPicker : public QWidget
{
    Q_OBJECT
public:
    struct Item {
        QString text, group, tip, data;
    };
    SearchPicker(const QList<Item> &items, QWidget *parent = nullptr);
    void popup(const QPoint &globalPos);

signals:
    void picked(const QString &data);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void filter(const QString &text);
    class QLineEdit *m_search;
    class QTreeWidget *m_tree;
};

// Disables every input widget below `root` (locked layer), except those with the "allowLocked" property.
void lockInputs(QWidget *root, bool locked);

// The number fields of the application. Press and slide left or right to change the value (one step per pixel;
// Shift: a tenth; Ctrl / ⌘: ten): no focus, no text cursor, no selection. A click without moving edits the text, all
// of it selected; once editing, the mouse places the cursor and selects as in any text field.
class NumberBox : public QDoubleSpinBox
{
public:
    explicit NumberBox(QWidget *parent = nullptr);
};
class IntBox : public QSpinBox
{
public:
    explicit IntBox(QWidget *parent = nullptr);
};

// The application's check box (it replaces QCheckBox everywhere): a small square that fills with the accent when it
// is on, as the switches of MadMapper or Ableton Live. It is a QCheckBox, so the code that reads it is unchanged.
class FlagBox : public QCheckBox
{
    Q_OBJECT
public:
    explicit FlagBox(QWidget *parent = nullptr);
    explicit FlagBox(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override;
    bool hitButton(const QPoint &pos) const override { return rect().contains(pos); }
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    bool m_hover = false;
};

// A checkable button (bold, italic…) that is plainly colored with the accent while it is on.
class ToggleButton : public QToolButton
{
    Q_OBJECT
public:
    explicit ToggleButton(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(30, 26); }

protected:
    void paintEvent(QPaintEvent *) override;
};
