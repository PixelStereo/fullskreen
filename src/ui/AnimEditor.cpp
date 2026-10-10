#include "AnimEditor.h"
#include "Widgets.h"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

static const QColor kCurve(86, 196, 255);
static const QColor kWave(224, 180, 58);
static const QColor kHandle(235, 235, 240);
static const QColor kPlayhead(76, 217, 100);

QString animWaveName(AnimWave w)
{
    switch (w) {
    case AnimWave::Sine: return QStringLiteral("Sine");
    case AnimWave::Triangle: return QStringLiteral("Triangle");
    case AnimWave::Saw: return QStringLiteral("Saw");
    case AnimWave::Square: return QStringLiteral("Step (square)");
    case AnimWave::Random: return QStringLiteral("Random");
    case AnimWave::SmoothRandom: return QStringLiteral("Smooth random");
    }
    return {};
}

QString animCurveName(int c)
{
    if (c == kAnimBezier) return QStringLiteral("Bézier (handles)");
    if (c == kAnimHold) return QStringLiteral("Hold (steps)");
    return Engine::easingNames().value(c); // the easings of the fades
}

void addParamMenu(QMenu *menu, const std::vector<Engine::AnimParam> &params,
                  const std::function<void(QMenu *, const Engine::AnimParam &, const QString &)> &item)
{
    std::vector<std::pair<QString, QMenu *>> subs; // in the order they come
    for (const Engine::AnimParam &p : params) {
        const int cut = p.label.lastIndexOf(QStringLiteral(" › "));
        QMenu *into = menu;
        QString text = p.label;
        if (cut >= 0) { // the category: up to the last " › " (none: at the top of the menu)
            const QString cat = p.label.left(cut);
            auto it = std::find_if(subs.begin(), subs.end(), [&](const auto &x) { return x.first == cat; });
            if (it == subs.end()) {
                subs.push_back({cat, menu->addMenu(cat)});
                it = subs.end() - 1;
            }
            into = it->second;
            text = p.label.mid(cut + 3);
        }
        item(into, p, text);
    }
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

static bool sameKeys(const std::vector<AnimKey> &a, const std::vector<AnimKey> &b)
{
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        const AnimKey &x = a[k], &y = b[k];
        if (x.t != y.t || x.v != y.v || x.curve != y.curve || x.isCurrentValue != y.isCurrentValue || x.inDt != y.inDt ||
            x.inDv != y.inDv || x.outDt != y.outDt || x.outDv != y.outDv)
            return false;
    }
    return true;
}

bool sameAnimation(const Animation &a, const Animation &b)
{
    if (a.duration != b.duration || a.loop != b.loop || a.repeat != b.repeat || a.speed != b.speed ||
        a.tracks.size() != b.tracks.size())
        return false;
    for (size_t k = 0; k < a.tracks.size(); ++k) {
        const AnimTrack &x = a.tracks[k], &y = b.tracks[k];
        if (x.layer != y.layer || x.param != y.param || x.enabled != y.enabled || x.oscillator != y.oscillator ||
            x.wave != y.wave || x.period != y.period || x.center != y.center || x.amplitude != y.amplitude ||
            x.phase != y.phase || x.seed != y.seed || !sameKeys(x.keys, y.keys))
            return false;
    }
    return true;
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

static void sortKeys(std::vector<AnimKey> &keys)
{
    std::stable_sort(keys.begin(), keys.end(), [](const AnimKey &a, const AnimKey &b) { return a.t < b.t; });
}

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
    if (m_dragKey < 0 && m_dragHandle < 0) update();
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

AnimTrack CurveLane::shown() const
{
    AnimTrack t = m_track;
    if (startsFromCurrent(t)) t.captured = m_live;
    return t;
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

bool CurveLane::handleAt(QPointF p, int *key, bool *out) const
{
    if (m_track.oscillator) return false;
    const AnimTrack t = shown();
    double bestD = 7;
    bool found = false;
    for (size_t k = 0; k + 1 < t.keys.size(); ++k) {
        if (t.keys[k].curve != kAnimBezier) continue;
        double t1, v1, t2, v2;
        t.bezierHandles(k, &t1, &v1, &t2, &v2);
        const double d1 = std::hypot(xOf(t1) - p.x(), yOf(v1) - p.y()), d2 = std::hypot(xOf(t2) - p.x(), yOf(v2) - p.y());
        if (d1 <= bestD) bestD = d1, *key = int(k), *out = true, found = true;
        if (d2 <= bestD) bestD = d2, *key = int(k + 1), *out = false, found = true;
    }
    return found;
}

void CurveLane::emitEdited(int gesture) { emit edited(m_track, gesture); }

void CurveLane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(26, 26, 30));
    // Beyond the animation (zoomed out past its ends): darker
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
    const AnimTrack drawn = shown();
    const bool any = m_track.oscillator || !m_track.keys.empty();
    if (any) {
        QPainterPath path;
        for (int x = int(x0); x <= int(std::ceil(x1)); ++x) {
            const double t = tOf(x);
            const QPointF pt(x, yOf(drawn.valueAt(t, t)));
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
    if (!m_track.oscillator) {
        // Bézier handles: a line from the key, a small ring at its end
        for (size_t k = 0; k + 1 < drawn.keys.size(); ++k) {
            if (drawn.keys[k].curve != kAnimBezier) continue;
            double t1, v1, t2, v2;
            drawn.bezierHandles(k, &t1, &v1, &t2, &v2);
            const QPointF a(xOf(drawn.keys[k].t), yOf(shownValue(k))), b(xOf(drawn.keys[k + 1].t), yOf(shownValue(k + 1)));
            const QPointF h1(xOf(t1), yOf(v1)), h2(xOf(t2), yOf(v2));
            p.setPen(QPen(QColor(200, 200, 210, 140), 1));
            p.drawLine(a, h1);
            p.drawLine(b, h2);
            for (auto [h, key, out] : {std::tuple{h1, int(k), true}, std::tuple{h2, int(k + 1), false}}) {
                const bool active = key == m_dragHandle && out == m_dragOut;
                p.setPen(QPen(active ? QColor(Qt::white) : kHandle, 1.4));
                p.setBrush(QColor(26, 26, 30));
                p.drawEllipse(h, 3.5, 3.5);
            }
        }
        // The keys (a held one is a square; a first key "current value" is a ring)
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
            if (key.curve == kAnimHold) p.drawRect(QRectF(c.x() - 3.5, c.y() - 3.5, 7, 7));
            else if (key.curve == kAnimBezier) p.drawPolygon(QPolygonF({c + QPointF(0, -5), c + QPointF(5, 0), c + QPointF(0, 5), c + QPointF(-5, 0)}));
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
    int hk = -1;
    bool out = false;
    if (handleAt(e->position(), &hk, &out)) {
        m_dragHandle = hk;
        m_dragOut = out;
        m_gesture = ++s_gestures;
        update();
        return;
    }
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
    sortKeys(m_track.keys);
    update();
    emitEdited(m_gesture);
}

// The handle of a key follows the cursor; the key's other handle turns with it (the same slope: the curve stays smooth
// through the key) unless `alone`
void CurveLane::dragHandle(QPointF pos, bool alone)
{
    const int k = m_dragHandle;
    const int n = int(m_track.keys.size());
    if (k < 0 || k >= n) return;
    AnimKey &key = m_track.keys[size_t(k)];
    const double t = m_t0 + (pos.x() - kMargin) / std::max(1, width() - 2 * kMargin) * (m_t1 - m_t0);
    const double v = m_vhi - (pos.y() - 8) / std::max(1, height() - 16) * (m_vhi - m_vlo);
    const double kv = shownValue(size_t(k));
    double dt = 0, dv = v - kv;
    if (m_dragOut) {
        if (k + 1 >= n) return;
        const double span = m_track.keys[size_t(k + 1)].t - key.t;
        dt = std::clamp(t - key.t, 1e-6, std::max(1e-6, span));
        key.outDt = dt;
        key.outDv = dv;
    } else {
        if (k < 1) return;
        const double span = key.t - m_track.keys[size_t(k - 1)].t;
        dt = std::clamp(t - key.t, -std::max(1e-6, span), -1e-6);
        key.inDt = dt;
        key.inDv = dv;
    }
    if (alone) return;
    const double slope = dv / dt;
    if (m_dragOut && k > 0 && m_track.keys[size_t(k - 1)].curve == kAnimBezier) {
        const double span = key.t - m_track.keys[size_t(k - 1)].t;
        const double odt = key.hasIn() ? key.inDt : -span / 3;
        key.inDt = std::min(-1e-6, odt);
        key.inDv = key.inDt * slope;
    } else if (!m_dragOut && key.curve == kAnimBezier && k + 1 < n) {
        const double span = m_track.keys[size_t(k + 1)].t - key.t;
        const double odt = key.hasOut() ? key.outDt : span / 3;
        key.outDt = std::max(1e-6, odt);
        key.outDv = key.outDt * slope;
    }
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
    if (m_dragHandle >= 0) {
        dragHandle(pos, e->modifiers() & Qt::AltModifier);
        update();
        emitEdited(m_gesture);
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
        sortKeys(keys);
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
    int hk = -1;
    bool out = false;
    if (handleAt(pos, &hk, &out)) {
        setCursor(Qt::SizeAllCursor);
        setToolTip(QStringLiteral("Bézier handle: drag to shape the curve (the key's other handle turns with it; Alt: on its own)"));
        return;
    }
    const int k = keyAt(pos);
    setCursor(m_track.oscillator ? Qt::ArrowCursor : k >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
    if (k >= 0) {
        const AnimKey &key = m_track.keys[size_t(k)];
        const QString v = k == 0 && key.isCurrentValue ? QStringLiteral("current value") : num(key.v);
        setToolTip(QStringLiteral("%1 s → %2 · %3\nDrag: move · Shift+drag: along one axis · double-click: delete")
                       .arg(key.t, 0, 'f', 2).arg(v, animCurveName(key.curve)));
    } else {
        setToolTip(m_track.oscillator ? QStringLiteral("A wave: its settings are with the track")
                                      : QStringLiteral("Click: a key · drag a key: move it · drag elsewhere: draw · "
                                                       "double-click a key: delete · right-click: curve (Bézier…), value\n"
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
    m_dragHandle = -1;
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
    // Shown without waiting here: the menu goes with the lane, should the rows be rebuilt meanwhile
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (k >= 0) {
        if (k == 0) {
            QAction *cur = menu->addAction(QStringLiteral("Start from the current value"), this, [this](bool on) {
                m_track.keys[0].isCurrentValue = on;
                if (on && m_current) m_track.keys[0].v = m_current(); // the placeholder, should it be turned off
                update();
                emitEdited();
            });
            cur->setCheckable(true);
            cur->setChecked(m_track.keys[0].isCurrentValue);
            cur->setToolTip(QStringLiteral("The number starts from where it is when the animation starts: no jump"));
            menu->addSeparator();
        }
        QMenu *curve = menu->addMenu(QStringLiteral("Curve to the next key"));
        for (int c : {0, 1, 2, 3, 4, 5, int(kAnimBezier), int(kAnimHold)}) {
            QAction *a = curve->addAction(animCurveName(c), this, [this, k, c] {
                m_track.keys[size_t(k)].curve = c;
                update();
                emitEdited();
            });
            a->setCheckable(true);
            a->setChecked(m_track.keys[size_t(k)].curve == c);
        }
        const AnimKey &key = m_track.keys[size_t(k)];
        if (key.hasIn() || key.hasOut()) {
            menu->addAction(QStringLiteral("Automatic handles"), this, [this, k] {
                AnimKey &x = m_track.keys[size_t(k)];
                x.inDt = x.inDv = x.outDt = x.outDv = 0;
                update();
                emitEdited();
            });
        }
        QAction *value = menu->addAction(QStringLiteral("Value…"), this, [this, k] {
            bool ok = false;
            const double v = QInputDialog::getDouble(this, QStringLiteral("Key"), QStringLiteral("Value"), m_track.keys[size_t(k)].v,
                                                     -1e9, 1e9, 3, &ok);
            if (!ok) return;
            m_track.keys[size_t(k)].v = v;
            update();
            emitEdited();
        });
        value->setEnabled(!(k == 0 && m_track.keys[0].isCurrentValue));
        menu->addAction(QStringLiteral("Time…"), this, [this, k] {
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
        menu->addAction(QStringLiteral("Delete"), this, [this, k] {
            m_track.keys.erase(m_track.keys.begin() + k);
            if (!m_track.keys.empty() && k == 0) m_track.keys[0].isCurrentValue = false;
            update();
            emitEdited();
        });
    } else {
        const double t = tOf(e->pos().x());
        QAction *cur = menu->addAction(QStringLiteral("Key here with the current value"), this, [this, t] {
            m_track.keys.push_back(AnimKey{t, m_current(), 3});
            sortKeys(m_track.keys);
            for (size_t k = 1; k < m_track.keys.size(); ++k) m_track.keys[k].isCurrentValue = false;
            update();
            emitEdited();
        });
        cur->setEnabled(bool(m_current));
        QAction *smooth = menu->addAction(QStringLiteral("Every key: Bézier"), this, [this] {
            for (AnimKey &k : m_track.keys) k.curve = kAnimBezier;
            update();
            emitEdited();
        });
        smooth->setEnabled(m_track.keys.size() >= 2);
        QAction *clear = menu->addAction(QStringLiteral("Clear the keys"), this, [this] {
            m_track.keys.clear();
            update();
            emitEdited();
        });
        clear->setEnabled(!m_track.keys.empty());
    }
    menu->popup(e->globalPos());
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
// TrackRow

static NumberBox *numberBox(double lo, double hi, int decimals, double step, const QString &suffix = {})
{
    auto *b = new NumberBox;
    b->setKeyboardTracking(false); // a value typed is taken once it is complete
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setSingleStep(step);
    b->setSuffix(suffix);
    return b;
}

TrackRow::TrackRow(AnimEditor *editor, int index) : m_ed(editor), m_e(editor->engine()), m_i(index) {}

AnimTrack &TrackRow::track() { return m_ed->m_edit.tracks[size_t(m_i)]; }
const AnimTrack &TrackRow::track() const { return m_ed->m_edit.tracks[size_t(m_i)]; }

void TrackRow::build()
{
    const bool side = m_ed->layout() == AnimEditor::Layout::Side;
    QBoxLayout *box = side ? static_cast<QBoxLayout *>(new QHBoxLayout(this)) : new QVBoxLayout(this);
    box->setContentsMargins(0, 2, 0, 2);
    box->setSpacing(side ? 0 : 3);
    auto *head = new QWidget;
    if (side) {
        head->setFixedWidth(m_ed->headerWidth());
        head->setObjectName("trackHead");
        head->setStyleSheet("#trackHead { background:#222226; border-right:1px solid #34343a; }");
        head->setAttribute(Qt::WA_StyledBackground);
    }
    auto *g = new QGridLayout(head);
    g->setContentsMargins(side ? 6 : 0, side ? 4 : 0, side ? 6 : 0, side ? 4 : 0);
    g->setHorizontalSpacing(4);
    g->setVerticalSpacing(3);
    buildHeader(g);
    const int r = g->count() ? g->rowCount() : 0;

    m_kind = new QComboBox;
    m_kind->addItem(QStringLiteral("Keys"));
    m_kind->addItem(QStringLiteral("Wave"));
    m_kind->setToolTip(QStringLiteral("Keys: drawn over the duration (the pattern repeats when it loops; Bézier, easings, holds)\n"
                                      "Wave: a waveform of its own period, going on without a jump across the loops"));
    m_wave = new QComboBox;
    for (int w = 0; w < kAnimWaveCount; ++w) m_wave->addItem(animWaveName(AnimWave(w)));
    m_wave->setToolTip(QStringLiteral("Saw from -amplitude to +amplitude: on a rotation of 180°, the layer turns round and round.\n"
                                      "Random: a new value each period, held; Smooth random: gliding from one to the next"));
    m_reseed = new QPushButton(QStringLiteral("⟳"));
    m_reseed->setFixedWidth(28);
    m_reseed->setToolTip(QStringLiteral("Other random values"));
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
    auto small = [](const QString &t, std::function<void()> reset) {
        auto *l = new ResetLabel(t, std::move(reset));
        l->setStyleSheet("color:#9a9aa0;");
        return l;
    };
    // Their resets: a period of a second, no phase, the way a wave goes around this number (Engine::waveAround)
    auto around = [this](bool center) {
        double c = 0, a = 0;
        m_e->waveAround(track().layer, track().param, &c, &a);
        (center ? m_center : m_amp)->setValue(center ? c : a);
    };
    og->addWidget(small(QStringLiteral("Period"), [this] { m_period->setValue(AnimTrack().period); }), 0, 0);
    og->addWidget(m_period, 0, 1);
    og->addWidget(small(QStringLiteral("Phase"), [this] { m_phase->setValue(AnimTrack().phase); }), 0, 2);
    og->addWidget(m_phase, 0, 3);
    og->addWidget(small(QStringLiteral("Center"), [around] { around(true); }), 1, 0);
    og->addWidget(m_center, 1, 1);
    og->addWidget(small(QStringLiteral("Amplitude"), [around] { around(false); }), 1, 2);
    og->addWidget(m_amp, 1, 3);
    og->setColumnStretch(1, 1);
    og->setColumnStretch(3, 1);
    auto *kindRow = new QHBoxLayout;
    kindRow->setSpacing(4);
    kindRow->addWidget(m_kind, 1);
    kindRow->addWidget(m_wave, 2);
    kindRow->addWidget(m_reseed);
    g->addLayout(kindRow, r, 0, 1, 4);
    g->addWidget(m_osc, r + 1, 0, 1, 4);
    if (side) g->setRowStretch(r + 2, 1);
    g->setColumnStretch(1, 1);
    m_lane = new CurveLane;
    m_lane->setMinimumHeight(side ? 70 : std::max(70, m_ed->laneHeight()));
    m_lane->setCurrentValue([this] { return currentValue(); });
    box->addWidget(head);
    box->addWidget(m_lane, 1);

    connect(m_kind, qOverload<int>(&QComboBox::activated), this, [this](int k) {
        const bool osc = k == 1;
        if (osc == track().oscillator) return;
        track().oscillator = osc;
        fill();
        changed();
    });
    connect(m_wave, qOverload<int>(&QComboBox::activated), this, [this](int k) {
        track().wave = AnimWave(k);
        fill();
        changed();
    });
    connect(m_reseed, &QPushButton::clicked, this, [this] {
        track().seed = QRandomGenerator::global()->generate() | 1u;
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
    connect(m_lane, &CurveLane::edited, this, [this](const AnimTrack &t, int gesture) {
        track().keys = t.keys;
        changed(false, gesture ? QStringLiteral("lane%1").arg(gesture) : QString());
    });
    connect(m_lane, &CurveLane::zoomTime, m_ed, &AnimEditor::zoomTime);
    connect(m_lane, &CurveLane::panTime, m_ed, &AnimEditor::panTime);
    connect(m_lane, &CurveLane::zoomValue, this, [this](double factor, double anchor) {
        AnimEditor::LaneView &lv = m_ed->laneView(m_i);
        double vlo = 0, vhi = 1;
        m_ed->viewOfLane(m_i, m_lane->fullLo(), m_lane->fullHi(), &vlo, &vhi);
        const double nlo = anchor - (anchor - vlo) / factor, nhi = anchor + (vhi - anchor) / factor;
        lv.zoom = std::clamp(lv.zoom * factor, 1.0, 1e6);
        lv.center = (nlo + nhi) / 2;
        applyView();
    });
    connect(m_lane, &CurveLane::panValue, this, [this](double dv) {
        AnimEditor::LaneView &lv = m_ed->laneView(m_i);
        double vlo = 0, vhi = 1;
        m_ed->viewOfLane(m_i, m_lane->fullLo(), m_lane->fullHi(), &vlo, &vhi);
        lv.center = (vlo + vhi) / 2 + dv;
        applyView();
    });
    fill();
}

void TrackRow::fill()
{
    m_filling = true;
    const AnimTrack &t = track();
    m_kind->setCurrentIndex(t.oscillator ? 1 : 0);
    m_wave->setCurrentIndex(int(t.wave));
    m_wave->setVisible(t.oscillator);
    m_reseed->setVisible(t.oscillator && (t.wave == AnimWave::Random || t.wave == AnimWave::SmoothRandom));
    m_osc->setVisible(t.oscillator);
    m_period->setValue(t.period);
    m_phase->setValue(t.phase);
    m_center->setValue(t.center);
    m_amp->setValue(t.amplitude);
    m_filling = false;
    updateLane();
}

void TrackRow::setPlayhead(double pos, bool shown) { m_lane->setPlayhead(pos, shown); }

void TrackRow::setLive(double captured, bool running)
{
    if (!m_lane->track().keys.empty() && m_lane->track().keys.front().isCurrentValue)
        m_lane->setLive(running && std::isfinite(captured) ? captured : currentValue());
}

std::pair<double, double> TrackRow::fullRange() const { return {m_lane->fullLo(), m_lane->fullHi()}; }

std::pair<double, double> TrackRow::range() const { return m_ed->rangeOf(track()); }

double TrackRow::currentValue() const
{
    const AnimTrack &t = track();
    double v = 0;
    if (!m_e->animParamValue(t.layer, t.param, &v)) v = range().first;
    return v;
}

void TrackRow::updateLane()
{
    const auto [lo, hi] = range();
    m_lane->setTrack(track(), m_ed->m_edit.duration, lo, hi);
    applyView();
}

void TrackRow::applyView()
{
    double vlo = 0, vhi = 1;
    m_ed->viewOfLane(m_i, m_lane->fullLo(), m_lane->fullHi(), &vlo, &vhi);
    m_lane->setView(m_ed->m_t0, m_ed->m_t1, vlo, vhi);
}

void TrackRow::changed(bool lane, const QString &mergeKey)
{
    if (lane) updateLane();
    m_ed->commit(QStringLiteral("Edit Animation"), mergeKey);
}

// ---------------------------------------------------------------------------
// AnimEditor

AnimEditor::AnimEditor(Engine *engine, QUndoStack *undo, Layout layout, bool scrollTracks, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo), m_layout(layout), m_scrollTracks(scrollTracks)
{
    const bool side = layout == Layout::Side;
    m_main = new QVBoxLayout(this);
    m_main->setContentsMargins(0, 0, 0, 0);
    m_body = new QWidget;
    auto *v = new QVBoxLayout(m_body);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(side ? 6 : 4);
    m_main->addWidget(m_body, 1);

    // Transport, duration, speed, loop
    m_play = new QPushButton(side ? QStringLiteral("▶ Play") : QStringLiteral("▶"));
    m_pause = new QPushButton(side ? QStringLiteral("❚❚ Pause") : QStringLiteral("❚❚"));
    m_stop = new QPushButton(side ? QStringLiteral("■ Stop") : QStringLiteral("■"));
    m_rewind = new QPushButton(QStringLiteral("⏮"));
    m_play->setToolTip(QStringLiteral("Play (from where it is; from the start once stopped)"));
    m_pause->setToolTip(QStringLiteral("Pause"));
    m_rewind->setToolTip(QStringLiteral("Rewind: back to the start (its values at once)"));
    m_stop->setToolTip(QStringLiteral("Stop: back to the start, the values stay where they are"));
    m_play->setStyleSheet("QPushButton:enabled { background:#2f6b3a; color:white; }");
    if (!side)
        for (QPushButton *b : {m_play, m_pause, m_stop, m_rewind}) b->setFixedWidth(34);
    m_time = new QLabel;
    m_time->setMinimumWidth(side ? 170 : 0);
    m_duration = numberBox(0.05, 36000, 2, 0.1, QStringLiteral(" s"));
    m_duration->setKeyboardTracking(false); // typing 12 over 4 does not first squash the keys into 1 s
    m_duration->setToolTip(QStringLiteral("Duration of one pass (the time of the keys; a wave has its own period)"));
    m_loop = new QComboBox;
    m_loop->addItem(QStringLiteral("Once"));
    m_loop->addItem(QStringLiteral("Loop"));
    m_loop->addItem(QStringLiteral("Ping-pong"));
    m_loop->setToolTip(QStringLiteral("Once: one pass, then it stops · Loop: again from the start · Ping-pong: back and forth"));
    m_repeat = new IntBox;
    m_repeat->setRange(0, 100000);
    m_repeat->setSpecialValueText(QStringLiteral("∞"));
    m_repeat->setPrefix(QStringLiteral("× "));
    m_repeat->setToolTip(QStringLiteral("Passes (∞: until it is stopped)"));
    m_speed = numberBox(0, 10, 2, 0.1);
    m_speed->setToolTip(QStringLiteral("Playback speed (1 = normal, 0 = frozen where it is)"));
    m_fit = new QPushButton(QStringLiteral("Fit"));
    m_fit->setToolTip(QStringLiteral("Zoom on the keys: time from the first to the last, each lane from its lowest value to its highest"));
    m_all = new QPushButton(QStringLiteral("All"));
    m_all->setToolTip(QStringLiteral("The whole duration, the whole range of each number"));
    auto gray = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setStyleSheet("color:#9a9aa0;");
        return l;
    };
    // The names of the duration and the speed: a click puts them back (4 s, 1×)
    auto durationLabel = [this](bool dim) {
        auto *l = new ResetLabel(QStringLiteral("Duration"), [this] { m_duration->setValue(Animation().duration); });
        if (dim) l->setStyleSheet("color:#9a9aa0;");
        return l;
    };
    auto speedLabel = [this](bool dim) {
        auto *l = new ResetLabel(QStringLiteral("Speed"), [this] { m_speed->setValue(Animation().speed); });
        if (dim) l->setStyleSheet("color:#9a9aa0;");
        return l;
    };
    if (side) {
        auto *transport = new QHBoxLayout;
        for (QWidget *w : std::initializer_list<QWidget *>{m_play, m_pause, m_stop, m_rewind, m_time}) transport->addWidget(w);
        transport->addStretch();
        transport->addWidget(durationLabel(false));
        transport->addWidget(m_duration);
        transport->addWidget(speedLabel(false));
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
        zoomRow->addWidget(gray(QStringLiteral("Zoom  Time")));
        zoomRow->addWidget(m_zoomX);
        zoomRow->addSpacing(8);
        zoomRow->addWidget(gray(QStringLiteral("Values")));
        zoomRow->addWidget(m_zoomY);
        zoomRow->addWidget(m_fit);
        zoomRow->addWidget(m_all);
        zoomRow->addStretch();
        v->addLayout(zoomRow);
    } else { // narrow: three short rows
        auto *r1 = new QHBoxLayout;
        r1->setSpacing(3);
        for (QWidget *w : std::initializer_list<QWidget *>{m_play, m_pause, m_stop, m_rewind}) r1->addWidget(w);
        r1->addSpacing(4);
        r1->addWidget(m_time, 1);
        v->addLayout(r1);
        auto *r2 = new QHBoxLayout;
        r2->addWidget(durationLabel(true));
        r2->addWidget(m_duration, 1);
        r2->addWidget(speedLabel(true));
        r2->addWidget(m_speed, 1);
        v->addLayout(r2);
        auto *r3 = new QHBoxLayout;
        r3->addWidget(m_loop, 1);
        r3->addWidget(m_repeat);
        r3->addSpacing(6);
        for (QPushButton *b : {m_fit, m_all}) {
            b->setFixedWidth(40);
            r3->addWidget(b);
        }
        v->addLayout(r3);
    }

    const int bar = scrollTracks ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0; // the lanes' scroll bar
    auto *rulerRow = new QHBoxLayout;
    rulerRow->setSpacing(0);
    rulerRow->addSpacing(headerWidth());
    m_ruler = new TimeRuler;
    rulerRow->addWidget(m_ruler, 1);
    rulerRow->addSpacing(bar);
    v->addLayout(rulerRow);
    auto *scrollRow = new QHBoxLayout;
    scrollRow->setSpacing(0);
    scrollRow->addSpacing(headerWidth());
    m_scrollX = new QScrollBar(Qt::Horizontal);
    m_scrollX->setToolTip(QStringLiteral("The time shown (Shift+wheel over a lane, or a middle drag)"));
    scrollRow->addWidget(m_scrollX, 1);
    scrollRow->addSpacing(bar);
    v->addLayout(scrollRow);
    if (scrollTracks) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *host = new QWidget;
        m_tracks = new QVBoxLayout(host);
        m_tracks->setContentsMargins(0, 0, 0, 0);
        m_tracks->setSpacing(1);
        m_tracks->addStretch();
        scroll->setWidget(host);
        v->addWidget(scroll, 1);
    } else {
        m_tracks = new QVBoxLayout;
        m_tracks->setContentsMargins(0, 0, 0, 0);
        m_tracks->setSpacing(1);
        v->addLayout(m_tracks, 1);
    }

    // Transport
    connect(m_play, &QPushButton::clicked, this, [this] { control(AnimAction::Play); });
    connect(m_pause, &QPushButton::clicked, this, [this] { control(AnimAction::Pause); });
    connect(m_stop, &QPushButton::clicked, this, [this] { control(AnimAction::Stop); });
    connect(m_rewind, &QPushButton::clicked, this, [this] { control(AnimAction::Rewind); });
    connect(m_ruler, &TimeRuler::seekRequested, this, [this](double t) {
        // Within the pass shown: from the pass it is in
        Animation a;
        if (!fetch(&a)) return;
        control(AnimAction::Seek, a.state == AnimState::Stopped ? t : a.clockAt(t));
    });
    connect(m_duration, &QDoubleSpinBox::valueChanged, this, [this](double d) {
        if (m_filling) return;
        const bool whole = m_t0 <= 1e-9 && m_t1 >= m_edit.duration - 1e-9; // the whole stays shown
        m_edit.duration = d;
        for (AnimTrack &t : m_edit.tracks)
            for (AnimKey &k : t.keys) k.t = std::min(k.t, d); // nothing beyond the end
        if (whole) m_t0 = 0, m_t1 = d;
        commit(QStringLiteral("Animation Duration"), QStringLiteral("duration"));
        m_ruler->setDuration(d);
        rebuildRows();
        applyView();
    });
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, [this](double s) {
        if (m_filling) return;
        m_edit.speed = s;
        commit(QStringLiteral("Animation Speed"), QStringLiteral("speed"));
    });
    connect(m_fit, &QPushButton::clicked, this, &AnimEditor::fitView);
    connect(m_all, &QPushButton::clicked, this, &AnimEditor::resetView);
    if (m_zoomX) {
        connect(m_zoomX, &QSlider::valueChanged, this, [this](int z) { setTimeZoom(std::pow(200.0, z / 100.0)); });
        connect(m_zoomY, &QSlider::valueChanged, this, [this](int z) {
            for (size_t k = 0; k < m_edit.tracks.size(); ++k) laneView(int(k)).zoom = std::pow(200.0, z / 100.0);
            applyView();
        });
    }
    connect(m_scrollX, &QScrollBar::valueChanged, this, [this](int x) {
        if (m_filling) return;
        const double span = m_t1 - m_t0;
        m_t0 = x / 1000.0;
        m_t1 = m_t0 + span;
        applyView();
    });
    connect(m_ruler, &TimeRuler::zoomTime, this, &AnimEditor::zoomTime);
    connect(m_ruler, &TimeRuler::panTime, this, &AnimEditor::panTime);
    connect(m_loop, qOverload<int>(&QComboBox::activated), this, [this](int k) {
        m_edit.loop = AnimLoop(k);
        m_repeat->setEnabled(k != 0);
        commit(QStringLiteral("Animation Loop"));
    });
    connect(m_repeat, &QSpinBox::valueChanged, this, [this](int n) {
        if (m_filling) return;
        m_edit.repeat = n;
        commit(QStringLiteral("Animation Repeat"), QStringLiteral("repeat"));
    });
    auto *timer = new QTimer(this);
    timer->setInterval(33);
    connect(timer, &QTimer::timeout, this, &AnimEditor::poll);
    timer->start();
}

const std::vector<Engine::AnimParam> &AnimEditor::paramsOf(quint64 layer)
{
    auto it = m_params.find(layer);
    if (it == m_params.end()) it = m_params.emplace(layer, m_engine->animatableParams(layer)).first;
    return it->second;
}

std::pair<double, double> AnimEditor::rangeOf(const AnimTrack &t)
{
    for (const Engine::AnimParam &p : paramsOf(t.layer))
        if (p.path == t.param) return {p.min, p.max};
    return {0.0, 1.0};
}

void AnimEditor::setEditorEnabled(bool on) { m_body->setEnabled(on); }

void AnimEditor::forgetView() { m_freshView = true; }

void AnimEditor::reload()
{
    Animation a;
    const bool has = fetch(&a);
    m_params.clear();
    // The same as edited (an edit made here, come back, or another number of the same layer): nothing to rebuild
    if (has && m_has && !m_freshView && sameAnimation(a, m_edit) && m_rows.size() == a.tracks.size()) {
        m_edit = a;
        loaded();
        poll();
        return;
    }
    m_has = has;
    m_edit = has ? a : Animation();
    setEditorEnabled(has);
    m_filling = true;
    m_duration->setValue(m_edit.duration);
    m_speed->setValue(m_edit.speed);
    m_loop->setCurrentIndex(int(m_edit.loop));
    m_repeat->setValue(m_edit.repeat);
    m_repeat->setEnabled(m_edit.loop != AnimLoop::Once);
    m_filling = false;
    m_ruler->setDuration(m_edit.duration);
    if (m_freshView) { // another animation: seen whole
        m_freshView = false;
        m_t0 = 0;
        m_t1 = std::max(0.05, m_edit.duration);
        m_laneViews.clear();
        if (m_zoomX) {
            const QSignalBlocker bx(m_zoomX), by(m_zoomY);
            m_zoomX->setValue(0);
            m_zoomY->setValue(0);
        }
    }
    m_laneViews.resize(m_edit.tracks.size());
    rebuildRows();
    applyView();
    loaded();
    poll();
}

void AnimEditor::rebuildRows()
{
    for (TrackRow *r : m_rows) {
        r->hide();
        r->deleteLater();
    }
    m_rows.clear();
    for (int k = 0; k < int(m_edit.tracks.size()); ++k) {
        TrackRow *r = makeRow(k);
        r->build();
        m_tracks->insertWidget(m_scrollTracks ? m_tracks->count() - 1 : m_tracks->count(), r, m_scrollTracks ? 0 : 1);
        m_rows.push_back(r);
    }
}

void AnimEditor::commit(const QString &text, const QString &mergeKey)
{
    Animation before;
    if (!fetch(&before)) return;
    // Not reloaded from the engine meanwhile: the rows (and a key being dragged) stay
    m_committing = true;
    store(before, m_edit, text, mergeKey);
    m_committing = false;
    emit edited();
}

void AnimEditor::poll()
{
    if (!isVisible()) return;
    Animation a;
    if (!fetch(&a)) {
        m_time->clear();
        return;
    }
    const bool shown = a.state != AnimState::Stopped;
    const double pos = a.position();
    m_ruler->setPlayhead(pos, shown);
    for (size_t k = 0; k < m_rows.size(); ++k) {
        m_rows[k]->setPlayhead(pos, shown);
        m_rows[k]->setLive(k < a.tracks.size() ? a.tracks[k].captured : std::numeric_limits<double>::quiet_NaN(), shown);
    }
    const double d = std::max(0.05, a.duration);
    const int pass = int(std::floor(std::min(a.clock, a.length()) / d)) + 1;
    const QString state = a.state == AnimState::Playing ? QStringLiteral("<span style='color:%1'>playing</span>").arg(kPlayhead.name())
                        : a.state == AnimState::Paused  ? QStringLiteral("<span style='color:#e0b43a'>paused</span>")
                                                        : QStringLiteral("<span style='color:#8a8a90'>stopped</span>");
    m_time->setText(QStringLiteral("<b>%1</b> / %2 s · %3%4")
                        .arg(pos, 0, 'f', 2)
                        .arg(d, 0, 'f', 2)
                        .arg(state)
                        .arg(shown && a.loop != AnimLoop::Once ? QStringLiteral(" · pass %1").arg(pass) : QString()));
    m_play->setEnabled(a.state != AnimState::Playing);
    m_pause->setEnabled(a.state == AnimState::Playing);
}

// ---------------------------------------------------------------------------
// Zoom

void AnimEditor::applyView()
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
        m_scrollX->setVisible(m_layout == Layout::Side || span < d - 1e-9); // narrow: only when zoomed in
        m_filling = false;
        if (m_zoomX) {
            const QSignalBlocker b(m_zoomX);
            m_zoomX->setValue(int(std::lround(std::log(d / span) / std::log(200.0) * 100)));
        }
    }
    for (TrackRow *r : m_rows) r->applyView();
}

void AnimEditor::zoomTime(double factor, double anchor)
{
    if (factor <= 0) return;
    const double span = (m_t1 - m_t0) / factor;
    m_t0 = anchor - (anchor - m_t0) / factor;
    m_t1 = m_t0 + span;
    applyView();
}

void AnimEditor::panTime(double seconds)
{
    m_t0 += seconds;
    m_t1 += seconds;
    applyView();
}

void AnimEditor::setTimeZoom(double factor)
{
    const double c = (m_t0 + m_t1) / 2, span = std::max(0.05, m_edit.duration) / std::max(1.0, factor);
    m_t0 = c - span / 2;
    m_t1 = c + span / 2;
    applyView();
}

AnimEditor::LaneView &AnimEditor::laneView(int track)
{
    if (track >= int(m_laneViews.size())) m_laneViews.resize(size_t(track) + 1);
    return m_laneViews[size_t(std::max(0, track))];
}

void AnimEditor::viewOfLane(int track, double lo, double hi, double *vlo, double *vhi)
{
    LaneView &lv = laneView(track);
    const double span = (hi - lo) / std::max(1.0, lv.zoom);
    double c = std::isnan(lv.center) ? (lo + hi) / 2 : lv.center;
    c = std::clamp(c, lo + span / 2, hi - span / 2);
    *vlo = c - span / 2;
    *vhi = c + span / 2;
}

void AnimEditor::fitView()
{
    // Time: from the first key to the last (waves: the whole)
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
    // Values: each lane from its lowest key to its highest (a wave: its swing)
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
    if (m_zoomY) {
        const QSignalBlocker b(m_zoomY);
        m_zoomY->setValue(0);
    }
    applyView();
}

void AnimEditor::resetView()
{
    m_t0 = 0;
    m_t1 = std::max(0.05, m_edit.duration);
    for (LaneView &lv : m_laneViews) lv = LaneView();
    if (m_zoomY) {
        const QSignalBlocker b(m_zoomY);
        m_zoomY->setValue(0);
    }
    applyView();
}
