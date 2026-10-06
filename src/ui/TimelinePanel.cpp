#include "TimelinePanel.h"
#include "Engine.h"
#include "Widgets.h"
#include "Commands.h"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QWheelEvent>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using AnimKey = Engine::AnimKey;
using AnimTrack = Engine::AnimTrack;

static const QColor kCurve(86, 196, 255);
static const QColor kWave(224, 180, 58);
static const QColor kPlayhead(76, 217, 100);
static constexpr int kHeader = 300; // width of a track's settings, left of its lane

static QString curveName(int c)
{
    switch (c) {
    case 0: return QStringLiteral("Linear");
    case 1: return QStringLiteral("Ease in");
    case 2: return QStringLiteral("Ease out");
    case 3: return QStringLiteral("Ease in-out");
    case 4: return QStringLiteral("Ease in (cubic)");
    case 5: return QStringLiteral("Ease out (cubic)");
    case Engine::kAnimHold: return QStringLiteral("Hold (steps)");
    }
    return {};
}

static QString num(double v)
{
    return std::abs(v) >= 100 ? QString::number(v, 'f', 0) : QString::number(v, 'f', std::abs(v) >= 10 ? 1 : 2);
}

// A step between the ticks of the seconds, at least `px` pixels apart
static double tickStep(double duration, double width, double px)
{
    for (double s : {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0})
        if (duration <= 0 || width / (duration / s) >= px) return s;
    return 1200;
}

// ---------------------------------------------------------------------------
// TimelineList

QStringList TimelineList::mimeTypes() const { return {QString::fromLatin1(kTimelineMime)}; }

QMimeData *TimelineList::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty()) return nullptr;
    auto *m = new QMimeData;
    m->setData(kTimelineMime, QByteArray::number(items.first()->data(Qt::UserRole).toULongLong()));
    return m;
}

// ---------------------------------------------------------------------------
// CurveLane

CurveLane::CurveLane(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(70);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

static bool startsFromCurrent(const AnimTrack &t) { return !t.oscillator && !t.keys.empty() && t.keys.front().isCurrentValue; }

void CurveLane::setTrack(const AnimTrack &t, double duration, double lo, double hi)
{
    m_track = t;
    m_duration = std::max(0.05, duration);
    // The range shows the parameter's, and anything beyond it the track holds
    if (t.oscillator) {
        lo = std::min(lo, t.center - std::abs(t.amplitude));
        hi = std::max(hi, t.center + std::abs(t.amplitude));
    } else {
        for (size_t k = 0; k < t.keys.size(); ++k) {
            if (k == 0 && t.keys[0].isCurrentValue) continue; // a placeholder
            lo = std::min(lo, t.keys[k].v);
            hi = std::max(hi, t.keys[k].v);
        }
    }
    if (hi - lo < 1e-9) hi = lo + 1;
    m_lo = lo;
    m_hi = hi;
    update();
}

void CurveLane::setView(double t0, double t1, double vlo, double vhi)
{
    if (t1 - t0 < 1e-6) t1 = t0 + 1e-6;
    if (vhi - vlo < 1e-12) vhi = vlo + 1e-12;
    if (t0 == m_t0 && t1 == m_t1 && vlo == m_vlo && vhi == m_vhi) return;
    m_t0 = t0;
    m_t1 = t1;
    m_vlo = vlo;
    m_vhi = vhi;
    update();
}

void CurveLane::setLive(double v)
{
    if (!startsFromCurrent(m_track)) return;
    if (v == m_live || (std::isnan(v) && std::isnan(m_live))) return;
    m_live = v;
    if (m_dragKey < 0) update();
}

void CurveLane::setPlayhead(double position, bool shown)
{
    if (std::abs(position - m_playhead) < 1e-6 && shown == m_showPlayhead) return;
    m_playhead = position;
    m_showPlayhead = shown;
    update();
}

double CurveLane::xOf(double t) const { return kMargin + (t - m_t0) / (m_t1 - m_t0) * (width() - 2 * kMargin); }
double CurveLane::tOf(double x) const
{
    return std::clamp(m_t0 + (x - kMargin) / std::max(1, width() - 2 * kMargin) * (m_t1 - m_t0), 0.0, m_duration);
}
double CurveLane::yOf(double v) const { return 8 + (m_vhi - v) / (m_vhi - m_vlo) * (height() - 16); }
double CurveLane::vOf(double y) const
{
    const double v = m_vhi - (y - 8) / std::max(1, height() - 16) * (m_vhi - m_vlo);
    return std::clamp(v, std::min(m_lo, m_vlo), std::max(m_hi, m_vhi));
}

double CurveLane::shownValue(size_t k) const
{
    if (k == 0 && startsFromCurrent(m_track) && std::isfinite(m_live)) return m_live;
    return m_track.keys[k].v;
}

int CurveLane::keyAt(QPointF p) const
{
    if (m_track.oscillator) return -1;
    int best = -1;
    double bestD = 8;
    for (size_t k = 0; k < m_track.keys.size(); ++k) {
        const double d = std::hypot(xOf(m_track.keys[k].t) - p.x(), yOf(shownValue(k)) - p.y());
        if (d <= bestD) best = int(k), bestD = d;
    }
    return best;
}

void CurveLane::emitEdited(int gesture) { emit edited(m_track, gesture); }

void CurveLane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(26, 26, 30));
    // Beyond the timeline (zoomed out past its ends): darker
    const double xs = xOf(0), xe = xOf(m_duration);
    if (xs > 0) p.fillRect(QRectF(0, 0, xs, height()), QColor(18, 18, 21));
    if (xe < width()) p.fillRect(QRectF(xe, 0, width() - xe, height()), QColor(18, 18, 21));
    const double x0 = std::max(xs, 0.0), x1 = std::min(xe, double(width()));
    // Seconds, and the values shown (their bounds and their middle, or zero)
    p.setPen(QColor(44, 44, 50));
    const double step = tickStep(m_t1 - m_t0, width() - 2 * kMargin, 50);
    for (long long i = std::max(0LL, (long long)std::ceil(m_t0 / step)); i * step <= std::min(m_t1, m_duration) + 1e-9; ++i)
        p.drawLine(QPointF(xOf(i * step), 0), QPointF(xOf(i * step), height()));
    const double mid = (m_vlo < 0 && m_vhi > 0) ? 0.0 : (m_vlo + m_vhi) / 2;
    for (double v : {m_vlo, mid, m_vhi}) p.drawLine(QPointF(x0, yOf(v)), QPointF(x1, yOf(v)));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    p.setPen(QColor(110, 110, 118));
    p.drawText(QRectF(x0 + 3, 1, 80, 12), Qt::AlignLeft | Qt::AlignTop, num(m_vhi));
    p.drawText(QRectF(x0 + 3, height() - 13, 80, 12), Qt::AlignLeft | Qt::AlignBottom, num(m_vlo));

    // The values over one pass
    const bool any = m_track.oscillator || !m_track.keys.empty();
    if (any) {
        AnimTrack shown = m_track;
        if (startsFromCurrent(shown)) shown.captured = m_live;
        QPainterPath path;
        for (int x = int(x0); x <= int(std::ceil(x1)); ++x) {
            const double t = tOf(x);
            const QPointF pt(x, yOf(shown.valueAt(t, t)));
            if (x == int(x0)) path.moveTo(pt);
            else path.lineTo(pt);
        }
        QColor c = m_track.oscillator ? kWave : kCurve;
        if (!m_track.enabled) c.setAlphaF(0.35);
        p.save();
        p.setClipRect(QRectF(x0, 0, x1 - x0, height()));
        p.setPen(QPen(c, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.restore();
    } else {
        p.setPen(QColor(110, 110, 118));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Click to add a key · drag to draw a pattern"));
    }
    // The keys (a held one is a square; a first key "current value" is a ring)
    if (!m_track.oscillator) {
        for (size_t k = 0; k < m_track.keys.size(); ++k) {
            const AnimKey &key = m_track.keys[k];
            const QPointF c(xOf(key.t), yOf(shownValue(k)));
            if (c.x() < -6 || c.x() > width() + 6) continue;
            const QColor fill = int(k) == m_dragKey ? QColor(Qt::white) : kCurve;
            if (k == 0 && key.isCurrentValue) {
                p.setPen(QPen(fill, 2));
                p.setBrush(QColor(26, 26, 30));
                p.drawEllipse(c, 5, 5);
                p.setPen(fill);
                p.drawText(QRectF(c.x() + 7, c.y() - 16, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("current"));
                continue;
            }
            p.setPen(QPen(QColor(20, 20, 24), 1));
            p.setBrush(fill);
            if (key.curve == Engine::kAnimHold) p.drawRect(QRectF(c.x() - 3.5, c.y() - 3.5, 7, 7));
            else p.drawEllipse(c, 4, 4);
        }
    }
    if (m_showPlayhead) {
        p.setPen(QPen(kPlayhead, 1.5));
        p.drawLine(QPointF(xOf(m_playhead), 0), QPointF(xOf(m_playhead), height()));
    }
}

static int s_gestures = 0; // numbers the drags of all lanes

void CurveLane::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::MiddleButton) { // the view moves (time and values)
        m_panning = true;
        m_panFrom = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || m_track.oscillator) return;
    const int k = keyAt(e->position());
    if (k >= 0) {
        m_dragKey = k;
        m_gesture = ++s_gestures;
        m_press = e->position();
        m_pressKey = m_track.keys[size_t(k)];
        update();
        return;
    }
    // Drawing: a key now, then one every few pixels while dragging
    m_drawing = true;
    m_gesture = ++s_gestures;
    m_base = m_track.keys;
    m_stroke = {AnimKey{tOf(e->position().x()), vOf(e->position().y()), 3}};
    m_lastDrawX = e->position().x();
    m_track.keys = m_base;
    m_track.keys.push_back(m_stroke.front());
    std::stable_sort(m_track.keys.begin(), m_track.keys.end(), [](const AnimKey &a, const AnimKey &b) { return a.t < b.t; });
    update();
    emitEdited(m_gesture);
}

void CurveLane::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();
    if (m_panning) {
        const QPointF d = pos - m_panFrom;
        m_panFrom = pos;
        emit panTime(-d.x() / std::max(1, width() - 2 * kMargin) * (m_t1 - m_t0));
        emit panValue(d.y() / std::max(1, height() - 16) * (m_vhi - m_vlo));
        return;
    }
    if (m_dragKey >= 0 && m_dragKey < int(m_track.keys.size())) {
        AnimKey &k = m_track.keys[size_t(m_dragKey)];
        // Between its neighbours: the keys keep their order. Shift: along one axis only (the one moved most)
        const double lo = m_dragKey > 0 ? m_track.keys[size_t(m_dragKey - 1)].t : 0.0;
        const double hi = m_dragKey + 1 < int(m_track.keys.size()) ? m_track.keys[size_t(m_dragKey + 1)].t : m_duration;
        const QPointF d = pos - m_press;
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        const bool timeOnly = (shift && std::abs(d.x()) >= std::abs(d.y())) || (m_dragKey == 0 && k.isCurrentValue);
        const bool valueOnly = shift && !timeOnly;
        k.t = valueOnly ? m_pressKey.t : std::clamp(tOf(xOf(m_pressKey.t) + d.x()), lo, hi);
        k.v = timeOnly ? m_pressKey.v : vOf(yOf(m_pressKey.v) + d.y());
        const QString v = m_dragKey == 0 && k.isCurrentValue ? QStringLiteral("current value") : num(k.v);
        QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("%1 s → %2").arg(k.t, 0, 'f', 2).arg(v), this);
        update();
        emitEdited(m_gesture);
        return;
    }
    if (m_drawing) {
        if (std::abs(pos.x() - m_lastDrawX) < 6) return;
        m_lastDrawX = pos.x();
        m_stroke.push_back(AnimKey{tOf(pos.x()), vOf(pos.y()), 0});
        // The keys under the stroke give way to the drawn ones, linear from point to point
        double a = m_stroke.front().t, b = a;
        for (const AnimKey &k : m_stroke) a = std::min(a, k.t), b = std::max(b, k.t);
        std::vector<AnimKey> keys;
        for (const AnimKey &k : m_base)
            if (k.t < a - 1e-9 || k.t > b + 1e-9) keys.push_back(k);
        for (AnimKey k : m_stroke) {
            k.curve = 0;
            keys.push_back(k);
        }
        std::stable_sort(keys.begin(), keys.end(), [](const AnimKey &x, const AnimKey &y) { return x.t < y.t; });
        // One key per time (going back over the stroke: the latest)
        std::vector<AnimKey> unique;
        for (const AnimKey &k : keys) {
            if (!unique.empty() && std::abs(unique.back().t - k.t) < 1e-6) unique.back() = k;
            else unique.push_back(k);
        }
        for (size_t k = 1; k < unique.size(); ++k) unique[k].isCurrentValue = false; // only a first key
        m_track.keys = unique;
        update();
        emitEdited(m_gesture);
        return;
    }
    const int k = keyAt(pos);
    setCursor(m_track.oscillator ? Qt::ArrowCursor : k >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
    if (k >= 0) {
        const AnimKey &key = m_track.keys[size_t(k)];
        const QString v = k == 0 && key.isCurrentValue ? QStringLiteral("current value") : num(key.v);
        setToolTip(QStringLiteral("%1 s → %2 · %3\nDrag: move · Shift+drag: along one axis · double-click: delete")
                       .arg(key.t, 0, 'f', 2).arg(v, curveName(key.curve)));
    } else {
        setToolTip(m_track.oscillator ? QStringLiteral("An oscillator: its settings are on the left")
                                      : QStringLiteral("Click: a key · drag a key: move it · drag elsewhere: draw · "
                                                       "double-click a key: delete · right-click: curve, value\n"
                                                       "⌘/Ctrl+wheel: zoom time · Alt+wheel: zoom values · "
                                                       "Shift+wheel or middle drag: move the view"));
    }
}

void CurveLane::mouseReleaseEvent(QMouseEvent *)
{
    if (m_panning) {
        m_panning = false;
        unsetCursor();
    }
    m_dragKey = -1;
    m_drawing = false;
    m_base.clear();
    m_stroke.clear();
    update();
}

void CurveLane::mouseDoubleClickEvent(QMouseEvent *e)
{
    const int k = keyAt(e->position());
    if (k < 0) return;
    m_track.keys.erase(m_track.keys.begin() + k);
    m_dragKey = -1;
    update();
    emitEdited();
}

void CurveLane::wheelEvent(QWheelEvent *e)
{
    const QPoint a = e->angleDelta();
    const double steps = (a.y() != 0 ? a.y() : a.x()) / 120.0;
    const Qt::KeyboardModifiers m = e->modifiers();
    if (m & Qt::ControlModifier) {
        emit zoomTime(std::pow(1.2, steps), tOf(e->position().x()));
    } else if (m & Qt::AltModifier) {
        const double v = m_vhi - (e->position().y() - 8) / std::max(1, height() - 16) * (m_vhi - m_vlo);
        emit zoomValue(std::pow(1.2, steps), v);
    } else if ((m & Qt::ShiftModifier) || (a.x() != 0 && std::abs(a.x()) > std::abs(a.y()))) {
        emit panTime(-steps * 0.1 * (m_t1 - m_t0));
    } else {
        e->ignore(); // the tracks scroll
        return;
    }
    e->accept();
}

void CurveLane::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_track.oscillator) return;
    const int k = keyAt(e->pos());
    QMenu menu;
    if (k >= 0) {
        if (k == 0) {
            QAction *cur = menu.addAction(QStringLiteral("Start from the current value"), this, [this](bool on) {
                m_track.keys[0].isCurrentValue = on;
                if (on && m_current) m_track.keys[0].v = m_current(); // the placeholder, should it be turned off
                update();
                emitEdited();
            });
            cur->setCheckable(true);
            cur->setChecked(m_track.keys[0].isCurrentValue);
            cur->setToolTip(QStringLiteral("The number starts from where it is when the timeline starts: no jump"));
            menu.addSeparator();
        }
        QMenu *curve = menu.addMenu(QStringLiteral("Curve to the next key"));
        for (int c : {0, 1, 2, 3, 4, 5, int(Engine::kAnimHold)}) {
            QAction *a = curve->addAction(curveName(c), this, [this, k, c] {
                m_track.keys[size_t(k)].curve = c;
                update();
                emitEdited();
            });
            a->setCheckable(true);
            a->setChecked(m_track.keys[size_t(k)].curve == c);
        }
        QAction *value = menu.addAction(QStringLiteral("Value…"), this, [this, k] {
            bool ok = false;
            const double v = QInputDialog::getDouble(this, QStringLiteral("Key"), QStringLiteral("Value"), m_track.keys[size_t(k)].v,
                                                     -1e9, 1e9, 3, &ok);
            if (!ok) return;
            m_track.keys[size_t(k)].v = v;
            update();
            emitEdited();
        });
        value->setEnabled(!(k == 0 && m_track.keys[0].isCurrentValue));
        menu.addAction(QStringLiteral("Time…"), this, [this, k] {
            bool ok = false;
            const double lo = k > 0 ? m_track.keys[size_t(k - 1)].t : 0.0;
            const double hi = k + 1 < int(m_track.keys.size()) ? m_track.keys[size_t(k + 1)].t : m_duration;
            const double t = QInputDialog::getDouble(this, QStringLiteral("Key"), QStringLiteral("Time (s)"), m_track.keys[size_t(k)].t,
                                                     lo, hi, 3, &ok);
            if (!ok) return;
            m_track.keys[size_t(k)].t = t;
            update();
            emitEdited();
        });
        menu.addAction(QStringLiteral("Delete"), this, [this, k] {
            m_track.keys.erase(m_track.keys.begin() + k);
            if (!m_track.keys.empty() && k == 0) m_track.keys[0].isCurrentValue = false;
            update();
            emitEdited();
        });
    } else {
        const double t = tOf(e->pos().x());
        QAction *cur = menu.addAction(QStringLiteral("Key here with the current value"), this, [this, t] {
            m_track.keys.push_back(AnimKey{t, m_current(), 3});
            std::stable_sort(m_track.keys.begin(), m_track.keys.end(), [](const AnimKey &a, const AnimKey &b) { return a.t < b.t; });
            for (size_t k = 1; k < m_track.keys.size(); ++k) m_track.keys[k].isCurrentValue = false;
            update();
            emitEdited();
        });
        cur->setEnabled(bool(m_current));
        QAction *clear = menu.addAction(QStringLiteral("Clear the keys"), this, [this] {
            m_track.keys.clear();
            update();
            emitEdited();
        });
        clear->setEnabled(!m_track.keys.empty());
    }
    menu.exec(e->globalPos());
}

// ---------------------------------------------------------------------------
// TimeRuler

TimeRuler::TimeRuler(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(22);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Click or drag: go to that time (the values follow) · ⌘/Ctrl+wheel: zoom · wheel: move"));
}

void TimeRuler::setDuration(double d)
{
    m_duration = std::max(0.05, d);
    update();
}

void TimeRuler::setView(double t0, double t1)
{
    m_t0 = t0;
    m_t1 = std::max(t0 + 1e-6, t1);
    update();
}

void TimeRuler::setPlayhead(double position, bool shown)
{
    if (std::abs(position - m_playhead) < 1e-6 && shown == m_shown) return;
    m_playhead = position;
    m_shown = shown;
    update();
}

double TimeRuler::tOf(double x) const
{
    const double m = CurveLane::kMargin;
    return std::clamp(m_t0 + (x - m) / std::max(1.0, width() - 2 * m) * (m_t1 - m_t0), 0.0, m_duration);
}

void TimeRuler::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(34, 34, 38));
    const double m = CurveLane::kMargin, w = width() - 2 * m;
    auto x = [&](double t) { return m + (t - m_t0) / (m_t1 - m_t0) * w; };
    const double step = tickStep(m_t1 - m_t0, w, 50);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    const int decimals = step < 0.5 ? 2 : step < 1 ? 1 : 0;
    for (long long i = std::max(0LL, (long long)std::ceil(m_t0 / step)); i * step <= std::min(m_t1, m_duration) + 1e-9; ++i) {
        const double t = i * step;
        p.setPen(QColor(90, 90, 98));
        p.drawLine(QPointF(x(t), height() - 6), QPointF(x(t), height()));
        p.setPen(QColor(150, 150, 158));
        p.drawText(QRectF(x(t) + 2, 0, 60, height() - 4), Qt::AlignLeft | Qt::AlignVCenter, QString::number(t, 'f', decimals));
    }
    if (m_shown) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(kPlayhead);
        p.setPen(Qt::NoPen);
        const double px = x(m_playhead);
        p.drawPolygon(QPolygonF({QPointF(px - 5, 0), QPointF(px + 5, 0), QPointF(px, 8)}));
        p.fillRect(QRectF(px - 0.75, 0, 1.5, height()), kPlayhead);
    }
}

void TimeRuler::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) emit seekRequested(tOf(e->position().x()));
}

void TimeRuler::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton) emit seekRequested(tOf(e->position().x()));
}

void TimeRuler::wheelEvent(QWheelEvent *e)
{
    const QPoint a = e->angleDelta();
    const double steps = (a.y() != 0 ? a.y() : a.x()) / 120.0;
    if (e->modifiers() & Qt::ControlModifier) emit zoomTime(std::pow(1.2, steps), tOf(e->position().x()));
    else emit panTime(-steps * 0.1 * (m_t1 - m_t0));
    e->accept();
}

// ---------------------------------------------------------------------------
// TrackRow: a track's settings (what it drives, curve or oscillator), and its lane

class TrackRow : public QWidget
{
public:
    TrackRow(TimelineWindow *w, int index);
    void setPlayhead(double pos, bool shown) { m_lane->setPlayhead(pos, shown); }
    void setLive(double captured, bool running) // what a "current value" first key starts from
    {
        if (!m_lane->track().keys.empty() && m_lane->track().keys.front().isCurrentValue)
            m_lane->setLive(running && std::isfinite(captured) ? captured : currentValue());
    }
    void applyView(); // the window's zoom
    std::pair<double, double> fullRange() const { return {m_lane->fullLo(), m_lane->fullHi()}; }

private:
    AnimTrack &track() { return m_w->m_edit.tracks[size_t(m_i)]; }
    void fill();        // the widgets from the track
    void fillParams();  // the numbers of its layer
    void updateLane();
    void changed(bool lane = true, const QString &mergeKey = {}); // the track edited: to the engine
    std::pair<double, double> range() const;
    double currentValue() const;
    TimelineWindow *m_w;
    Engine *m_e;
    int m_i;
    std::vector<Engine::AnimParam> m_params;
    FlagBox *m_on;
    QComboBox *m_layer, *m_kind, *m_wave;
    QPushButton *m_param; // its menu: the categories, each one a sub-menu of its numbers
    void selectParam(const QString &path);
    QWidget *m_osc;
    NumberBox *m_period, *m_center, *m_amp, *m_phase;
    CurveLane *m_lane;
    bool m_filling = false;
};

static NumberBox *numberBox(double lo, double hi, int decimals, double step, const QString &suffix = {})
{
    auto *b = new NumberBox;
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setSingleStep(step);
    b->setSuffix(suffix);
    return b;
}

TrackRow::TrackRow(TimelineWindow *w, int index) : m_w(w), m_e(w->m_engine), m_i(index)
{
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 2, 0, 2);
    h->setSpacing(0);
    auto *head = new QWidget;
    head->setFixedWidth(kHeader);
    head->setObjectName("trackHead");
    head->setStyleSheet("#trackHead { background:#222226; border-right:1px solid #34343a; }");
    head->setAttribute(Qt::WA_StyledBackground);
    auto *g = new QGridLayout(head);
    g->setContentsMargins(6, 4, 6, 4);
    g->setHorizontalSpacing(4);
    g->setVerticalSpacing(3);
    m_on = new FlagBox;
    m_on->setToolTip(QStringLiteral("On: the track drives its number while the timeline plays"));
    m_layer = new QComboBox;
    m_layer->setToolTip(QStringLiteral("The layer driven (or the composition)"));
    auto *del = new QPushButton(QStringLiteral("✕"));
    del->setFixedWidth(26);
    del->setToolTip(QStringLiteral("Delete the track"));
    m_param = new QPushButton;
    m_param->setToolTip(QStringLiteral("The number driven: a category, then the number in it"));
    m_param->setMenu(new QMenu(m_param));
    m_param->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding-left:6px; }"));
    m_kind = new QComboBox;
    m_kind->addItem(QStringLiteral("Curve"));
    m_kind->addItem(QStringLiteral("Oscillator"));
    m_kind->setToolTip(QStringLiteral("Curve: keys drawn over the duration (the pattern repeats when the timeline loops)\n"
                                      "Oscillator: a wave of its own period, going on without a jump across the loops"));
    m_wave = new QComboBox;
    for (const char *n : {"Sine", "Triangle", "Saw", "Square"}) m_wave->addItem(QString::fromLatin1(n));
    m_wave->setToolTip(QStringLiteral("Saw from -amplitude to +amplitude: on a rotation of 180°, the layer turns round and round"));
    m_osc = new QWidget;
    auto *og = new QGridLayout(m_osc);
    og->setContentsMargins(0, 0, 0, 0);
    og->setHorizontalSpacing(4);
    og->setVerticalSpacing(3);
    m_period = numberBox(0.01, 36000, 2, 0.1, QStringLiteral(" s"));
    m_period->setToolTip(QStringLiteral("Seconds for one wave"));
    m_phase = numberBox(0, 1, 2, 0.05);
    m_phase->setToolTip(QStringLiteral("Where the wave starts (0..1 of a period)"));
    m_center = numberBox(-1e6, 1e6, 3, 0.01);
    m_center->setToolTip(QStringLiteral("The value the wave goes around"));
    m_amp = numberBox(-1e6, 1e6, 3, 0.01);
    m_amp->setToolTip(QStringLiteral("How far it goes each way"));
    auto small = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setStyleSheet("color:#9a9aa0;");
        return l;
    };
    og->addWidget(small(QStringLiteral("Period")), 0, 0);
    og->addWidget(m_period, 0, 1);
    og->addWidget(small(QStringLiteral("Phase")), 0, 2);
    og->addWidget(m_phase, 0, 3);
    og->addWidget(small(QStringLiteral("Center")), 1, 0);
    og->addWidget(m_center, 1, 1);
    og->addWidget(small(QStringLiteral("Amplitude")), 1, 2);
    og->addWidget(m_amp, 1, 3);
    g->addWidget(m_on, 0, 0);
    g->addWidget(m_layer, 0, 1, 1, 2);
    g->addWidget(del, 0, 3);
    g->addWidget(m_param, 1, 0, 1, 4);
    g->addWidget(m_kind, 2, 0, 1, 2);
    g->addWidget(m_wave, 2, 2, 1, 2);
    g->addWidget(m_osc, 3, 0, 1, 4);
    g->setRowStretch(4, 1);
    g->setColumnStretch(1, 1);
    m_lane = new CurveLane;
    m_lane->setCurrentValue([this] { return currentValue(); });
    h->addWidget(head);
    h->addWidget(m_lane, 1);

    connect(m_on, &QCheckBox::toggled, this, [this](bool on) {
        if (m_filling) return;
        track().enabled = on;
        changed();
    });
    connect(m_layer, qOverload<int>(&QComboBox::activated), this, [this](int) {
        track().layer = m_layer->currentData().toULongLong();
        fillParams();
        // Its first number, from where it is
        track().param = m_params.empty() ? QString() : m_params.front().path;
        track().keys = {AnimKey{0, currentValue(), 3}};
        fill();
        changed();
    });
    connect(m_kind, qOverload<int>(&QComboBox::activated), this, [this](int k) {
        const bool osc = k == 1;
        if (osc == track().oscillator) return;
        track().oscillator = osc;
        fill();
        changed();
    });
    connect(m_wave, qOverload<int>(&QComboBox::activated), this, [this](int k) {
        track().wave = Engine::AnimWave(k);
        changed();
    });
    auto value = [this](NumberBox *b, double AnimTrack::*field, const char *name) {
        connect(b, &QDoubleSpinBox::valueChanged, this, [this, field, name](double v) {
            if (m_filling) return;
            track().*field = v;
            changed(true, QStringLiteral("osc%1/%2").arg(m_i).arg(QLatin1String(name))); // the arrows: one step
        });
    };
    value(m_period, &AnimTrack::period, "period");
    value(m_phase, &AnimTrack::phase, "phase");
    value(m_center, &AnimTrack::center, "center");
    value(m_amp, &AnimTrack::amplitude, "amplitude");
    connect(del, &QPushButton::clicked, this, [this] {
        auto &tracks = m_w->m_edit.tracks;
        if (m_i < int(tracks.size())) tracks.erase(tracks.begin() + m_i);
        if (m_i < int(m_w->m_laneViews.size())) m_w->m_laneViews.erase(m_w->m_laneViews.begin() + m_i);
        m_w->commit(QStringLiteral("Delete Track"));
        QTimer::singleShot(0, m_w, [w = m_w] { w->rebuildRows(); }); // not from inside this row
    });
    connect(m_lane, &CurveLane::edited, this, [this](const AnimTrack &t, int gesture) {
        track().keys = t.keys;
        changed(false, gesture ? QStringLiteral("lane%1").arg(gesture) : QString());
    });
    connect(m_lane, &CurveLane::zoomTime, w, &TimelineWindow::zoomTime);
    connect(m_lane, &CurveLane::panTime, w, &TimelineWindow::panTime);
    connect(m_lane, &CurveLane::zoomValue, this, [this](double factor, double anchor) {
        TimelineWindow::LaneView &lv = m_w->laneView(m_i);
        double vlo = 0, vhi = 1;
        m_w->viewOfLane(m_i, m_lane->fullLo(), m_lane->fullHi(), &vlo, &vhi);
        const double nlo = anchor - (anchor - vlo) / factor, nhi = anchor + (vhi - anchor) / factor;
        lv.zoom = std::clamp(lv.zoom * factor, 1.0, 1e6);
        lv.center = (nlo + nhi) / 2;
        applyView();
    });
    connect(m_lane, &CurveLane::panValue, this, [this](double dv) {
        TimelineWindow::LaneView &lv = m_w->laneView(m_i);
        double vlo = 0, vhi = 1;
        m_w->viewOfLane(m_i, m_lane->fullLo(), m_lane->fullHi(), &vlo, &vhi);
        lv.center = (vlo + vhi) / 2 + dv;
        applyView();
    });

    // The layers (top first), then the composition
    m_layer->addItem(QStringLiteral("Composition"), QVariant::fromValue<quint64>(0));
    {
        Engine::Lock lk(&m_e->mutex());
        for (int i = 0; i < m_e->layerCount(); ++i) {
            const Layer *l = m_e->layer(i);
            m_layer->addItem((l->isViewport ? QStringLiteral("▣ ") : l->isGroup ? QStringLiteral("▤ ") : QString()) + l->name,
                             QVariant::fromValue<quint64>(l->id));
        }
    }
    fillParams();
    fill();
}

// The category of a number: its label up to " › " (none: shown at the top of the menu)
static QString paramCategory(const QString &label)
{
    const int k = label.indexOf(QStringLiteral(" › "));
    return k < 0 ? QString() : label.left(k);
}

void TrackRow::fillParams()
{
    m_params = m_e->animatableParams(track().layer);
    QMenu *menu = m_param->menu();
    menu->clear();
    std::vector<std::pair<QString, QMenu *>> subs; // in the order they come
    for (const Engine::AnimParam &p : m_params) {
        const QString cat = paramCategory(p.label);
        QMenu *into = menu;
        QString text = p.label;
        if (!cat.isEmpty()) {
            auto it = std::find_if(subs.begin(), subs.end(), [&](const auto &x) { return x.first == cat; });
            if (it == subs.end()) {
                subs.push_back({cat, menu->addMenu(cat)});
                it = subs.end() - 1;
            }
            into = it->second;
            text = p.label.mid(cat.size() + 3);
        }
        QAction *a = into->addAction(text, this, [this, path = p.path] { selectParam(path); });
        a->setCheckable(true);
        a->setChecked(p.path == track().param);
    }
}

void TrackRow::selectParam(const QString &path)
{
    track().param = path;
    const auto [lo, hi] = range();
    track().keys = {AnimKey{0, currentValue(), 3}};
    track().center = path == "mapping/rotation" ? 0 : (lo + hi) / 2;
    track().amplitude = path == "mapping/rotation" ? 180 : (hi - lo) / 2;
    fillParams(); // the check mark
    fill();
    changed();
}

void TrackRow::fill()
{
    m_filling = true;
    const AnimTrack &t = track();
    m_on->setChecked(t.enabled);
    int li = m_layer->findData(QVariant::fromValue<quint64>(t.layer));
    if (li < 0) { // the layer is gone: said so, the track is kept
        m_layer->addItem(QStringLiteral("(layer gone)"), QVariant::fromValue<quint64>(t.layer));
        li = m_layer->count() - 1;
    }
    m_layer->setCurrentIndex(li);
    QString label = t.param.isEmpty() ? QStringLiteral("(choose a number)") : QStringLiteral("(%1 — not on this layer)").arg(t.param);
    for (const Engine::AnimParam &p : m_params)
        if (p.path == t.param) label = p.label;
    m_param->setText(label);
    m_kind->setCurrentIndex(t.oscillator ? 1 : 0);
    m_wave->setCurrentIndex(int(t.wave));
    m_wave->setVisible(t.oscillator);
    m_osc->setVisible(t.oscillator);
    m_period->setValue(t.period);
    m_phase->setValue(t.phase);
    m_center->setValue(t.center);
    m_amp->setValue(t.amplitude);
    m_filling = false;
    updateLane();
}

std::pair<double, double> TrackRow::range() const
{
    const AnimTrack &t = m_w->m_edit.tracks[size_t(m_i)];
    for (const Engine::AnimParam &p : m_params)
        if (p.path == t.param) return {p.min, p.max};
    return {0.0, 1.0};
}

double TrackRow::currentValue() const
{
    const AnimTrack &t = m_w->m_edit.tracks[size_t(m_i)];
    double v = 0;
    if (!m_e->animParamValue(t.layer, t.param, &v)) v = range().first;
    return v;
}

void TrackRow::updateLane()
{
    const auto [lo, hi] = range();
    m_lane->setTrack(track(), m_w->m_edit.duration, lo, hi);
    applyView();
}

void TrackRow::applyView()
{
    double vlo = 0, vhi = 1;
    m_w->viewOfLane(m_i, m_lane->fullLo(), m_lane->fullHi(), &vlo, &vhi);
    m_lane->setView(m_w->m_t0, m_w->m_t1, vlo, vhi);
}

void TrackRow::changed(bool lane, const QString &mergeKey)
{
    if (lane) updateLane();
    m_w->commit(QStringLiteral("Edit Timeline Track"), mergeKey);
}

// ---------------------------------------------------------------------------
// TimelineWindow

TimelineWindow::TimelineWindow(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_engine(engine), m_undo(undo)
{
    setWindowTitle(QStringLiteral("Timelines"));
    resize(1120, 560);
    auto *h = new QHBoxLayout(this);

    // The list
    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("<b>Timelines</b>")));
    m_list = new TimelineList;
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setToolTip(QStringLiteral("Drag a timeline onto a step of a sequence (Play by default) · double-click to rename"));
    left->addWidget(m_list, 1);
    auto *lb = new QHBoxLayout;
    m_add = new QPushButton(QStringLiteral("+"));
    m_add->setToolTip(QStringLiteral("New timeline"));
    m_dup = new QPushButton(QStringLiteral("Duplicate"));
    m_del = new QPushButton(QStringLiteral("−"));
    m_del->setToolTip(QStringLiteral("Delete the timeline (the steps that drive it then do nothing)"));
    for (QPushButton *b : {m_add, m_dup, m_del}) lb->addWidget(b);
    left->addLayout(lb);
    auto *hint = new QLabel(QStringLiteral("Drag a timeline onto a step of a sequence: the step plays it (or pauses, "
                                           "stops, rewinds, seeks, changes its loop)."));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8a8a90; font-size:11px;");
    left->addWidget(hint);
    h->addLayout(left, 1);

    // The one selected
    m_editor = new QWidget;
    auto *v = new QVBoxLayout(m_editor);
    v->setContentsMargins(0, 0, 0, 0);
    auto *transport = new QHBoxLayout;
    m_play = new QPushButton(QStringLiteral("▶ Play"));
    m_pause = new QPushButton(QStringLiteral("❚❚ Pause"));
    m_stop = new QPushButton(QStringLiteral("■ Stop"));
    m_rewind = new QPushButton(QStringLiteral("⏮"));
    m_rewind->setToolTip(QStringLiteral("Rewind: back to the start (its values at once)"));
    m_stop->setToolTip(QStringLiteral("Stop: back to the start, the values stay where they are"));
    m_play->setStyleSheet("QPushButton:enabled { background:#2f6b3a; color:white; }");
    m_time = new QLabel;
    m_time->setMinimumWidth(170);
    m_duration = numberBox(0.05, 36000, 2, 0.1, QStringLiteral(" s"));
    m_duration->setToolTip(QStringLiteral("Duration of one pass"));
    m_loop = new QComboBox;
    m_loop->addItem(QStringLiteral("Once"));
    m_loop->addItem(QStringLiteral("Loop"));
    m_loop->addItem(QStringLiteral("Ping-pong"));
    m_repeat = new IntBox;
    m_repeat->setRange(0, 100000);
    m_repeat->setSpecialValueText(QStringLiteral("∞"));
    m_repeat->setPrefix(QStringLiteral("× "));
    m_repeat->setToolTip(QStringLiteral("Passes (∞: until it is stopped)"));
    m_speed = numberBox(0, 10, 2, 0.1);
    m_speed->setToolTip(QStringLiteral("Playback speed (1 = normal, 0 = frozen where it is)"));
    for (QWidget *w : std::initializer_list<QWidget *>{m_play, m_pause, m_stop, m_rewind, m_time}) transport->addWidget(w);
    transport->addStretch();
    transport->addWidget(new QLabel(QStringLiteral("Duration")));
    transport->addWidget(m_duration);
    transport->addWidget(new QLabel(QStringLiteral("Speed")));
    transport->addWidget(m_speed);
    transport->addWidget(m_loop);
    transport->addWidget(m_repeat);
    v->addLayout(transport);

    // The zoom: time (all the lanes) and values (each lane its own; the slider sets them all)
    auto *zoomRow = new QHBoxLayout;
    m_zoomX = new QSlider(Qt::Horizontal);
    m_zoomY = new QSlider(Qt::Horizontal);
    for (QSlider *z : {m_zoomX, m_zoomY}) {
        z->setRange(0, 100);
        z->setMaximumWidth(160);
    }
    m_zoomX->setToolTip(QStringLiteral("Horizontal zoom: the time shown (⌘/Ctrl+wheel over a lane or the ruler)"));
    m_zoomY->setToolTip(QStringLiteral("Vertical zoom: the values shown in every lane (Alt+wheel over a lane: that lane only)"));
    m_fit = new QPushButton(QStringLiteral("Fit"));
    m_fit->setToolTip(QStringLiteral("Zoom on the keys: time from the first to the last, each lane from its lowest value to its highest"));
    m_all = new QPushButton(QStringLiteral("All"));
    m_all->setToolTip(QStringLiteral("The whole duration, the whole range of each number"));
    auto *zl = new QLabel(QStringLiteral("Zoom  Time"));
    zl->setStyleSheet("color:#9a9aa0;");
    auto *vl = new QLabel(QStringLiteral("Values"));
    vl->setStyleSheet("color:#9a9aa0;");
    zoomRow->addWidget(zl);
    zoomRow->addWidget(m_zoomX);
    zoomRow->addSpacing(8);
    zoomRow->addWidget(vl);
    zoomRow->addWidget(m_zoomY);
    zoomRow->addWidget(m_fit);
    zoomRow->addWidget(m_all);
    zoomRow->addStretch();
    v->addLayout(zoomRow);

    auto *rulerRow = new QHBoxLayout;
    rulerRow->setSpacing(0);
    rulerRow->addSpacing(kHeader);
    m_ruler = new TimeRuler;
    rulerRow->addWidget(m_ruler, 1);
    rulerRow->addSpacing(style()->pixelMetric(QStyle::PM_ScrollBarExtent)); // the lanes' scroll bar
    v->addLayout(rulerRow);
    auto *scrollRow = new QHBoxLayout;
    scrollRow->setSpacing(0);
    scrollRow->addSpacing(kHeader);
    m_scrollX = new QScrollBar(Qt::Horizontal);
    m_scrollX->setToolTip(QStringLiteral("The time shown (Shift+wheel over a lane, or a middle drag)"));
    scrollRow->addWidget(m_scrollX, 1);
    scrollRow->addSpacing(style()->pixelMetric(QStyle::PM_ScrollBarExtent));
    v->addLayout(scrollRow);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tracksHost = new QWidget;
    m_tracks = new QVBoxLayout(m_tracksHost);
    m_tracks->setContentsMargins(0, 0, 0, 0);
    m_tracks->setSpacing(1);
    m_tracks->addStretch();
    scroll->setWidget(m_tracksHost);
    v->addWidget(scroll, 1);
    auto *bottom = new QHBoxLayout;
    m_addTrack = new QPushButton(QStringLiteral("+ Track"));
    m_addTrack->setToolTip(QStringLiteral("A track for the layer selected in the layer list"));
    bottom->addWidget(m_addTrack);
    auto *how = new QLabel(QStringLiteral("Lane: click: a key · drag a key: move it (Shift: one axis) · drag elsewhere: draw · "
                                          "double-click a key: delete · right-click: curve, value, start from the current value"));
    how->setStyleSheet("color:#8a8a90; font-size:11px;");
    bottom->addWidget(how, 1);
    v->addLayout(bottom);
    h->addWidget(m_editor, 4);

    // The list
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling) return;
        m_current = it ? it->data(Qt::UserRole).toULongLong() : 0;
        loadEditor();
    });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling) return;
        const int i = m_engine->indexOfAnimation(it->data(Qt::UserRole).toULongLong());
        if (i < 0) return;
        const Engine::Animation before = m_engine->animation(i);
        if (before.name == it->text()) return;
        Engine::Animation a = before;
        a.name = it->text();
        if (a.id == m_current) m_edit.name = a.name;
        m_committing = true;
        if (m_undo) m_undo->push(new cmd::SetAnimation(m_engine, a.id, before, a, QStringLiteral("Rename Timeline")));
        else m_engine->setAnimation(i, a);
        m_committing = false;
        emit edited();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        Engine::Animation a;
        a.name = QStringLiteral("Timeline %1").arg(m_engine->animationCount() + 1);
        auto *c = new cmd::AddAnimation(m_engine, a, m_engine->animationCount(), QStringLiteral("New Timeline"));
        if (m_undo) m_undo->push(c);
        else { c->redo(); }
        m_current = c->animationId();
        if (!m_undo) delete c;
        refreshList();
        loadEditor();
        emit edited();
    });
    connect(m_dup, &QPushButton::clicked, this, [this] {
        const int i = m_engine->indexOfAnimation(m_current);
        if (i < 0) return;
        Engine::Animation a = m_engine->animation(i);
        a.id = 0;
        a.name += QStringLiteral(" copy");
        auto *c = new cmd::AddAnimation(m_engine, a, i + 1, QStringLiteral("Duplicate Timeline"));
        if (m_undo) m_undo->push(c);
        else { c->redo(); }
        m_current = c->animationId();
        if (!m_undo) delete c;
        refreshList();
        loadEditor();
        emit edited();
    });
    connect(m_del, &QPushButton::clicked, this, [this] {
        const int i = m_engine->indexOfAnimation(m_current);
        if (i < 0) return;
        if (m_undo) m_undo->push(new cmd::RemoveAnimation(m_engine, i));
        else m_engine->removeAnimation(i);
        emit edited();
    });

    // Transport
    connect(m_play, &QPushButton::clicked, this, [this] { m_engine->controlAnimation(m_current, Engine::AnimAction::Play); });
    connect(m_pause, &QPushButton::clicked, this, [this] { m_engine->controlAnimation(m_current, Engine::AnimAction::Pause); });
    connect(m_stop, &QPushButton::clicked, this, [this] { m_engine->controlAnimation(m_current, Engine::AnimAction::Stop); });
    connect(m_rewind, &QPushButton::clicked, this, [this] { m_engine->controlAnimation(m_current, Engine::AnimAction::Rewind); });
    connect(m_ruler, &TimeRuler::seekRequested, this, [this](double t) {
        // Within the pass shown: from the pass it is in
        const Engine::Animation a = m_engine->animation(m_engine->indexOfAnimation(m_current));
        m_engine->controlAnimation(m_current, Engine::AnimAction::Seek,
                                   a.state == Engine::AnimState::Stopped ? t : a.clockAt(t));
    });
    connect(m_duration, &QDoubleSpinBox::valueChanged, this, [this](double d) {
        if (m_filling) return;
        const bool whole = m_t0 <= 1e-9 && m_t1 >= m_edit.duration - 1e-9; // the whole stays shown
        m_edit.duration = d;
        for (AnimTrack &t : m_edit.tracks)
            for (AnimKey &k : t.keys) k.t = std::min(k.t, d); // nothing beyond the end
        if (whole) m_t0 = 0, m_t1 = d;
        commit(QStringLiteral("Timeline Duration"), QStringLiteral("duration"));
        m_ruler->setDuration(d);
        rebuildRows();
        applyView();
    });
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, [this](double s) {
        if (m_filling) return;
        m_edit.speed = s;
        commit(QStringLiteral("Timeline Speed"), QStringLiteral("speed"));
    });
    connect(m_fit, &QPushButton::clicked, this, &TimelineWindow::fitView);
    connect(m_all, &QPushButton::clicked, this, &TimelineWindow::resetView);
    connect(m_zoomX, &QSlider::valueChanged, this, [this](int z) { setTimeZoom(std::pow(200.0, z / 100.0)); });
    connect(m_zoomY, &QSlider::valueChanged, this, [this](int z) {
        for (size_t k = 0; k < m_edit.tracks.size(); ++k) laneView(int(k)).zoom = std::pow(200.0, z / 100.0);
        applyView();
    });
    connect(m_scrollX, &QScrollBar::valueChanged, this, [this](int x) {
        if (m_filling) return;
        const double span = m_t1 - m_t0;
        m_t0 = x / 1000.0;
        m_t1 = m_t0 + span;
        applyView();
    });
    connect(m_ruler, &TimeRuler::zoomTime, this, &TimelineWindow::zoomTime);
    connect(m_ruler, &TimeRuler::panTime, this, &TimelineWindow::panTime);
    connect(m_loop, qOverload<int>(&QComboBox::activated), this, [this](int k) {
        m_edit.loop = Engine::AnimLoop(k);
        m_repeat->setEnabled(k != 0);
        commit(QStringLiteral("Timeline Loop"));
    });
    connect(m_repeat, &QSpinBox::valueChanged, this, [this](int n) {
        if (m_filling) return;
        m_edit.repeat = n;
        commit(QStringLiteral("Timeline Repeat"), QStringLiteral("repeat"));
    });
    connect(m_addTrack, &QPushButton::clicked, this, [this] {
        if (m_engine->indexOfAnimation(m_current) < 0) return;
        AnimTrack t;
        quint64 layer = m_currentLayer;
        if (m_engine->indexOfId(layer) < 0) { // none selected: the first layer that is not a viewport
            layer = 0;
            Engine::Lock lk(&m_engine->mutex());
            for (int i = 0; i < m_engine->layerCount() && !layer; ++i)
                if (!m_engine->layer(i)->isViewport) layer = m_engine->layer(i)->id;
        }
        t.layer = layer;
        t.param = layer ? QStringLiteral("opacity") : QStringLiteral("level");
        double v = 1;
        m_engine->animParamValue(t.layer, t.param, &v);
        t.keys = {AnimKey{0, v, 3}};
        m_edit.tracks.push_back(t);
        commit(QStringLiteral("Add Track"));
        rebuildRows();
    });

    connect(m_engine, &Engine::animationsChanged, this, [this] {
        if (m_committing) return;
        refreshList();
        loadEditor();
    });
    connect(m_engine, &Engine::layersChanged, this, [this] {
        if (!(QApplication::mouseButtons() & Qt::LeftButton)) rebuildRows(); // the names of the layers
    });
    auto *timer = new QTimer(this);
    timer->setInterval(33);
    connect(timer, &QTimer::timeout, this, &TimelineWindow::poll);
    timer->start();
    refreshList();
    loadEditor();
}

void TimelineWindow::refreshList()
{
    m_filling = true;
    m_list->clear();
    const int n = m_engine->animationCount();
    if (m_engine->indexOfAnimation(m_current) < 0) m_current = n > 0 ? m_engine->animation(0).id : 0;
    for (int i = 0; i < n; ++i) {
        const Engine::Animation a = m_engine->animation(i);
        auto *it = new QListWidgetItem(a.name, m_list);
        it->setData(Qt::UserRole, QVariant::fromValue<quint64>(a.id));
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        if (a.id == m_current) m_list->setCurrentItem(it);
    }
    m_filling = false;
}

void TimelineWindow::loadEditor()
{
    const int i = m_engine->indexOfAnimation(m_current);
    m_editor->setEnabled(i >= 0);
    m_dup->setEnabled(i >= 0);
    m_del->setEnabled(i >= 0);
    m_edit = i >= 0 ? m_engine->animation(i) : Engine::Animation();
    m_filling = true;
    m_duration->setValue(m_edit.duration);
    m_speed->setValue(m_edit.speed);
    m_loop->setCurrentIndex(int(m_edit.loop));
    m_repeat->setValue(m_edit.repeat);
    m_repeat->setEnabled(m_edit.loop != Engine::AnimLoop::Once);
    m_filling = false;
    m_ruler->setDuration(m_edit.duration);
    if (m_viewOf != m_current) { // another timeline: seen whole
        m_viewOf = m_current;
        m_t0 = 0;
        m_t1 = std::max(0.05, m_edit.duration);
        m_laneViews.clear();
        const QSignalBlocker bx(m_zoomX), by(m_zoomY);
        m_zoomX->setValue(0);
        m_zoomY->setValue(0);
    }
    m_laneViews.resize(m_edit.tracks.size());
    rebuildRows();
    applyView();
    poll();
}

void TimelineWindow::rebuildRows()
{
    for (TrackRow *r : m_rows) r->deleteLater();
    m_rows.clear();
    for (int k = 0; k < int(m_edit.tracks.size()); ++k) {
        auto *r = new TrackRow(this, k);
        m_tracks->insertWidget(m_tracks->count() - 1, r); // before the stretch
        m_rows.push_back(r);
    }
}

void TimelineWindow::commit(const QString &text, const QString &mergeKey)
{
    const int i = m_engine->indexOfAnimation(m_current);
    if (i < 0) return;
    // Not reloaded from the engine meanwhile: the rows (and a key being dragged) stay
    m_committing = true;
    if (m_undo) m_undo->push(new cmd::SetAnimation(m_engine, m_edit.id, m_engine->animation(i), m_edit, text, mergeKey));
    else m_engine->setAnimation(i, m_edit);
    m_committing = false;
    emit edited();
}

void TimelineWindow::poll()
{
    if (!isVisible()) return;
    // The ones playing, in the list
    for (int k = 0; k < m_list->count(); ++k) {
        QListWidgetItem *it = m_list->item(k);
        const Engine::Animation a = m_engine->animation(m_engine->indexOfAnimation(it->data(Qt::UserRole).toULongLong()));
        const QBrush b = a.state == Engine::AnimState::Playing ? QBrush(kPlayhead) : QBrush();
        if (it->foreground() != b) it->setForeground(b);
    }
    const int i = m_engine->indexOfAnimation(m_current);
    if (i < 0) {
        m_time->clear();
        return;
    }
    const Engine::Animation a = m_engine->animation(i);
    const bool shown = a.state != Engine::AnimState::Stopped;
    const double pos = a.position();
    m_ruler->setPlayhead(pos, shown);
    for (size_t k = 0; k < m_rows.size(); ++k) {
        m_rows[k]->setPlayhead(pos, shown);
        m_rows[k]->setLive(k < a.tracks.size() ? a.tracks[k].captured : std::numeric_limits<double>::quiet_NaN(), shown);
    }
    const double d = std::max(0.05, a.duration);
    const int pass = int(std::floor(std::min(a.clock, a.length()) / d)) + 1;
    const QString state = a.state == Engine::AnimState::Playing ? QStringLiteral("<span style='color:%1'>playing</span>").arg(kPlayhead.name())
                        : a.state == Engine::AnimState::Paused  ? QStringLiteral("<span style='color:#e0b43a'>paused</span>")
                                                                : QStringLiteral("<span style='color:#8a8a90'>stopped</span>");
    m_time->setText(QStringLiteral("<b>%1</b> / %2 s · %3%4")
                        .arg(pos, 0, 'f', 2)
                        .arg(d, 0, 'f', 2)
                        .arg(state)
                        .arg(shown && a.loop != Engine::AnimLoop::Once ? QStringLiteral(" · pass %1").arg(pass) : QString()));
    m_play->setEnabled(a.state != Engine::AnimState::Playing);
    m_pause->setEnabled(a.state == Engine::AnimState::Playing);
}

// ---------------------------------------------------------------------------
// Zoom

void TimelineWindow::applyView()
{
    const double d = std::max(0.05, m_edit.duration);
    const double span = std::clamp(m_t1 - m_t0, std::min(0.02, d), d);
    m_t0 = std::clamp(m_t0, 0.0, d - span);
    m_t1 = m_t0 + span;
    m_ruler->setView(m_t0, m_t1);
    {
        m_filling = true;
        m_scrollX->setRange(0, int(std::lround((d - span) * 1000)));
        m_scrollX->setPageStep(std::max(1, int(std::lround(span * 1000))));
        m_scrollX->setSingleStep(std::max(1, int(std::lround(span * 100))));
        m_scrollX->setValue(int(std::lround(m_t0 * 1000)));
        m_scrollX->setEnabled(span < d - 1e-9);
        m_filling = false;
        const QSignalBlocker b(m_zoomX);
        m_zoomX->setValue(int(std::lround(std::log(d / span) / std::log(200.0) * 100)));
    }
    for (TrackRow *r : m_rows) r->applyView();
}

void TimelineWindow::zoomTime(double factor, double anchor)
{
    if (factor <= 0) return;
    const double span = (m_t1 - m_t0) / factor;
    m_t0 = anchor - (anchor - m_t0) / factor;
    m_t1 = m_t0 + span;
    applyView();
}

void TimelineWindow::panTime(double seconds)
{
    m_t0 += seconds;
    m_t1 += seconds;
    applyView();
}

void TimelineWindow::setTimeZoom(double factor)
{
    const double c = (m_t0 + m_t1) / 2, span = std::max(0.05, m_edit.duration) / std::max(1.0, factor);
    m_t0 = c - span / 2;
    m_t1 = c + span / 2;
    applyView();
}

TimelineWindow::LaneView &TimelineWindow::laneView(int track)
{
    if (track >= int(m_laneViews.size())) m_laneViews.resize(size_t(track) + 1);
    return m_laneViews[size_t(std::max(0, track))];
}

void TimelineWindow::viewOfLane(int track, double lo, double hi, double *vlo, double *vhi)
{
    LaneView &lv = laneView(track);
    const double span = (hi - lo) / std::max(1.0, lv.zoom);
    double c = std::isnan(lv.center) ? (lo + hi) / 2 : lv.center;
    c = std::clamp(c, lo + span / 2, hi - span / 2);
    *vlo = c - span / 2;
    *vhi = c + span / 2;
}

void TimelineWindow::fitView()
{
    // Time: from the first key to the last (oscillators: the whole)
    const double d = std::max(0.05, m_edit.duration);
    double a = d, b = 0;
    bool whole = false;
    for (const AnimTrack &t : m_edit.tracks) {
        if (t.oscillator) whole = true;
        else if (!t.keys.empty()) a = std::min(a, t.keys.front().t), b = std::max(b, t.keys.back().t);
    }
    if (whole || b - a < 1e-6) a = 0, b = d;
    const double pad = std::max(0.02 * d, (b - a) * 0.05);
    m_t0 = a - pad;
    m_t1 = b + pad;
    // Values: each lane from its lowest key to its highest (an oscillator: its swing)
    for (size_t k = 0; k < m_edit.tracks.size() && k < m_rows.size(); ++k) {
        const AnimTrack &t = m_edit.tracks[k];
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        if (t.oscillator) {
            lo = t.center - std::abs(t.amplitude);
            hi = t.center + std::abs(t.amplitude);
        } else {
            for (size_t i = 0; i < t.keys.size(); ++i) {
                if (i == 0 && t.keys[0].isCurrentValue) continue;
                lo = std::min(lo, t.keys[i].v);
                hi = std::max(hi, t.keys[i].v);
            }
        }
        LaneView &lv = laneView(int(k));
        const auto [flo, fhi] = m_rows[k]->fullRange();
        if (!std::isfinite(lo) || !std::isfinite(hi)) {
            lv = LaneView();
            continue;
        }
        double span = (hi - lo) * 1.15;
        if (span < (fhi - flo) * 1e-3) span = (fhi - flo) * 0.1; // one value: around it
        lv.zoom = std::clamp((fhi - flo) / span, 1.0, 1e6);
        lv.center = (lo + hi) / 2;
    }
    {
        const QSignalBlocker b(m_zoomY);
        m_zoomY->setValue(0);
    }
    applyView();
}

void TimelineWindow::resetView()
{
    m_t0 = 0;
    m_t1 = std::max(0.05, m_edit.duration);
    for (LaneView &lv : m_laneViews) lv = LaneView();
    const QSignalBlocker b(m_zoomY);
    m_zoomY->setValue(0);
    applyView();
}
