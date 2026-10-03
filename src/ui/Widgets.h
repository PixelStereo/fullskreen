#pragma once
// Small interface building blocks shared by the inspector and the parameter panels.

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QRectF>
#include <QWidget>
#include <functional>

class QDoubleSpinBox;
class QSlider;
class QLineEdit;

// Parameter name: a click puts the parameter back to its default value.
class ResetLabel : public QLabel
{
    Q_OBJECT
public:
    ResetLabel(const QString &text, std::function<void()> reset, QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    std::function<void()> m_reset;
};

// Playback bar of a video or a sound: the whole media, the in / out range highlighted with its two markers,
// and the playhead. Click or drag: seek. Drag a marker: move the in or out point.
class SeekBar : public QWidget
{
    Q_OBJECT
public:
    explicit SeekBar(QWidget *parent = nullptr);
    void setDuration(double d);
    void setPosition(double t);
    void setInOut(double in, double out); // out < 0: end of the media
    bool isDragging() const { return m_drag != None; }
    void setMarkersEditable(bool on) { m_markersEditable = on; } // locked layer: seek only
    QSize sizeHint() const override { return QSize(200, 30); }
    QSize minimumSizeHint() const override { return QSize(60, 30); }

signals:
    void seekRequested(double t);
    void inOutEdited(bool in, double t); // continuous while dragging a marker

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    QRectF track() const;
    double xOf(double t) const;
    double tOf(double x) const;
    enum Drag { None, Head, In, Out } m_drag = None;
    double m_duration = 0, m_pos = 0, m_in = 0, m_out = -1;
    bool m_markersEditable = true;
};

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
        QSlider *slider = nullptr;
        QDoubleSpinBox *spin = nullptr;
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
