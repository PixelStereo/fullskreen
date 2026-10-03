#include "MappingView.h"

#include <algorithm>
#include "Commands.h"
#include "Engine.h"

#include <QUndoStack>

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QToolButton>
#include <QWheelEvent>
#include <cmath>

static const QColor kSelected(255, 150, 40);
static const QColor kHandle(255, 255, 255, 230);
static const QColor kOutline(255, 150, 40, 220);
static const QColor kOutlineOther(255, 255, 255, 70);
static const QColor kGrid(255, 255, 255, 110);

MappingView::MappingView(Engine *engine, QWidget *parent) : QOpenGLWidget(parent), m_engine(engine)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setMinimumSize(320, 200);

    // Zoom buttons, top right of the preview
    m_zoomBar = new QWidget(this);
    m_zoomBar->setStyleSheet("QWidget { background:rgba(20,20,22,200); border-radius:4px; }"
                             "QToolButton { color:#ddd; min-width:24px; padding:2px 6px; }"
                             "QToolButton:hover { background:#3a3a3e; } QLabel { color:#aaa; padding:0 4px; }");
    auto *h = new QHBoxLayout(m_zoomBar);
    h->setContentsMargins(3, 2, 3, 2);
    h->setSpacing(1);
    auto button = [&](const QString &text, const QString &tip) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        h->addWidget(b);
        return b;
    };
    QToolButton *out = button(QStringLiteral("−"), QStringLiteral("Zoom out (mouse wheel)"));
    m_zoomLabel = new QLabel;
    m_zoomLabel->setMinimumWidth(44);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    h->addWidget(m_zoomLabel);
    QToolButton *in = button(QStringLiteral("+"), QStringLiteral("Zoom in (mouse wheel)"));
    QToolButton *fit = button(QStringLiteral("Fit"), QStringLiteral("Whole composition in view\n"
                                                                     "Pan: middle button, Alt/⌥ + drag, or two fingers on a trackpad"));
    connect(out, &QToolButton::clicked, this, [this] { zoomBy(1 / 1.25); });
    connect(in, &QToolButton::clicked, this, [this] { zoomBy(1.25); });
    connect(fit, &QToolButton::clicked, this, &MappingView::zoomToFit);
    updateZoomLabel();
}

void MappingView::updateZoomLabel()
{
    m_zoomLabel->setText(QStringLiteral("%1%").arg(int(std::lround(m_zoom * 100))));
    m_zoomBar->adjustSize();
    m_zoomBar->move(width() - m_zoomBar->width() - 8, 8);
}

void MappingView::resizeEvent(QResizeEvent *e)
{
    QOpenGLWidget::resizeEvent(e);
    updateZoomLabel();
}

// The point of the composition under `widgetPos` stays under it.
void MappingView::zoomAt(QPointF widgetPos, double factor)
{
    const QPointF n = toNorm(widgetPos);
    m_zoom = std::clamp(m_zoom * factor, 0.25, 64.0);
    const QRectF fit = fitRect();
    const double w = fit.width() * m_zoom, h = fit.height() * m_zoom;
    const QPointF center(widgetPos.x() - n.x() * w + w / 2, widgetPos.y() - n.y() * h + h / 2);
    m_pan = center - fit.center();
    updateZoomLabel();
    update();
}

void MappingView::zoomBy(double factor) { zoomAt(QPointF(width() / 2.0, height() / 2.0), factor); }

void MappingView::zoomToFit()
{
    m_zoom = 1.0;
    m_pan = QPointF();
    updateZoomLabel();
    update();
}

void MappingView::wheelEvent(QWheelEvent *e)
{
    // Trackpad (continuous scroll): two fingers pan, pinch or Ctrl/⌘ + scroll zooms.
    // Mouse wheel: zooms around the cursor.
    const bool trackpad = e->phase() != Qt::NoScrollPhase && !e->pixelDelta().isNull();
    if (trackpad && !(e->modifiers() & Qt::ControlModifier)) {
        m_pan += QPointF(e->pixelDelta());
        update();
    } else {
        const double steps = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->pixelDelta().y() * 2.0;
        zoomAt(e->position(), std::pow(1.0015, steps));
    }
    e->accept();
}

bool MappingView::event(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) { // macOS trackpad pinch
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomAt(g->position(), 1.0 + g->value());
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}



MappingView::~MappingView()
{
    makeCurrent();
    m_draw.destroy();
    doneCurrent();
}

void MappingView::setLayer(int index)
{
    if (index != m_layer) {
        m_selection.clear();
        m_primary = Handle{};
    }
    m_layer = index;
    update();
}

// An audio layer has no picture: nothing to map.
static bool hasPicture(const Layer *l) { return l && l->hasPicture(); }

Mapping *MappingView::mapping() const
{
    Layer *l = m_engine->layer(m_layer);
    return hasPicture(l) ? &l->mapping : nullptr;
}

const Mapping *MappingView::groupMapping(int layer) const
{
    const int g = m_engine->groupIndexOf(layer);
    if (g < 0) return nullptr;
    const Mapping &m = m_engine->layer(g)->mapping;
    return m.isIdentity() ? nullptr : &m;
}

QPointF MappingView::outOf(int layer, QPointF p) const
{
    const Mapping *g = groupMapping(layer);
    return g ? g->map(p.x(), p.y()) : p;
}

QPointF MappingView::canvasOf(int layer, QPointF p) const
{
    const Mapping *g = groupMapping(layer);
    return g ? g->unmap(p, p) : p;
}

QRectF MappingView::viewRect() const
{
    const QRectF fit = fitRect();
    QRectF r(0, 0, fit.width() * m_zoom, fit.height() * m_zoom);
    r.moveCenter(fit.center() + m_pan);
    return r;
}

QRectF MappingView::fitRect() const
{
    const QSize comp = m_engine->compositionSize();
    const double margin = 24;
    const double aw = width() - 2 * margin, ah = height() - 2 * margin;
    if (aw <= 0 || ah <= 0 || comp.isEmpty()) return QRectF(0, 0, width(), height());
    const double s = std::min(aw / comp.width(), ah / comp.height());
    const double w = comp.width() * s, h = comp.height() * s;
    return QRectF((width() - w) / 2, (height() - h) / 2, w, h);
}

QPointF MappingView::toWidget(QPointF n) const
{
    const QRectF r = viewRect();
    return QPointF(r.left() + n.x() * r.width(), r.top() + n.y() * r.height());
}

QPointF MappingView::toNorm(QPointF p) const
{
    const QRectF r = viewRect();
    return QPointF((p.x() - r.left()) / r.width(), (p.y() - r.top()) / r.height());
}

void MappingView::pushLine(std::vector<float> &v, QPointF a, QPointF b) const
{
    auto ndc = [&](QPointF p) {
        v.push_back(float(p.x() / width() * 2.0 - 1.0));
        v.push_back(float(1.0 - p.y() / height() * 2.0));
    };
    ndc(a);
    ndc(b);
}

void MappingView::pushRect(std::vector<float> &v, QPointF c, float half) const
{
    const QPointF a(c.x() - half, c.y() - half), b(c.x() + half, c.y() - half), d(c.x() - half, c.y() + half),
        e(c.x() + half, c.y() + half);
    for (QPointF p : {a, b, d, b, e, d}) {
        v.push_back(float(p.x() / width() * 2.0 - 1.0));
        v.push_back(float(1.0 - p.y() / height() * 2.0));
    }
}

std::vector<QPointF> MappingView::outline(const Mapping &m, int steps) const
{
    std::vector<QPointF> pts;
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(double(k) / steps, 0));
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(1, double(k) / steps));
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(1 - double(k) / steps, 1));
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(0, 1 - double(k) / steps));
    return pts;
}

void MappingView::initializeGL() { m_draw.init(); }

void MappingView::paintGL()
{
    auto f = context()->extraFunctions();
    const qreal dpr = devicePixelRatioF();
    f->glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    f->glClearColor(0.10f, 0.10f, 0.11f, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);

    const QRectF r = viewRect();
    // The OpenGL viewport origin is at the bottom left.
    f->glViewport(int(std::round(r.left() * dpr)), int(std::round((height() - r.bottom()) * dpr)),
                  int(std::round(r.width() * dpr)), int(std::round(r.height() * dpr)));
    m_draw.drawTexture(m_engine->outputTexture());
    f->glViewport(0, 0, int(width() * dpr), int(height() * dpr));

    // Composition frame
    std::vector<float> frame;
    pushLine(frame, r.topLeft(), r.topRight());
    pushLine(frame, r.topRight(), r.bottomRight());
    pushLine(frame, r.bottomRight(), r.bottomLeft());
    pushLine(frame, r.bottomLeft(), r.topLeft());
    m_draw.drawLines(frame, QColor(255, 255, 255, 40));

    // Read layers under the lock (the render thread uses them concurrently)
    Engine::Lock lk(&m_engine->mutex());

    // Outlines of the other layers
    if (m_showAll) {
        std::vector<float> others;
        for (int i = 0; i < m_engine->layerCount(); ++i) {
            if (i == m_layer) continue;
            Layer *l = m_engine->layer(i);
            if (!l->visible || !hasPicture(l)) continue;
            auto pts = outline(l->mapping, 16);
            for (QPointF &q : pts) q = outOf(i, q);
            for (size_t k = 0; k < pts.size(); ++k)
                pushLine(others, toWidget(pts[k]), toWidget(pts[(k + 1) % pts.size()]));
        }
        m_draw.drawLines(others, kOutlineOther);
    }

    Mapping *m = mapping();
    if (!m) return;
    const int cur = m_layer;
    auto W = [&](QPointF p) { return toWidget(outOf(cur, p)); }; // canvas of the layer -> widget

    // Warp mesh
    if (m->meshMode) {
        std::vector<float> grid;
        const int steps = 24;
        for (int j = 0; j < m->rows; ++j) {
            const double v = double(j) / (m->rows - 1);
            for (int k = 0; k < steps; ++k)
                pushLine(grid, W(m->map(double(k) / steps, v)), W(m->map(double(k + 1) / steps, v)));
        }
        for (int i = 0; i < m->cols; ++i) {
            const double u = double(i) / (m->cols - 1);
            for (int k = 0; k < steps; ++k)
                pushLine(grid, W(m->map(u, double(k) / steps)), W(m->map(u, double(k + 1) / steps)));
        }
        m_draw.drawLines(grid, kGrid);
    }

    std::vector<float> line;
    auto pts = outline(*m, 32);
    for (size_t k = 0; k < pts.size(); ++k) pushLine(line, W(pts[k]), W(pts[(k + 1) % pts.size()]));
    // Faint diagonals in corners mode, to judge the perspective
    if (!m->meshMode) {
        pushLine(line, W(m->corners[0]), W(m->corners[2]));
        pushLine(line, W(m->corners[1]), W(m->corners[3]));
    }
    // Locked layer: outline in red, no handles
    if (m_engine->isLocked(m_layer)) {
        m_draw.drawLines(line, QColor(230, 80, 70, 220));
        return;
    }
    m_draw.drawLines(line, kOutline);

    std::vector<float> handles, sel, shadow;
    auto addHandle = [&](const Handle &h, float half) {
        const QPointF p = W(handlePos(h));
        pushRect(shadow, p, half + 1.5f);
        pushRect(isSelected(h) ? sel : handles, p, half);
    };
    if (m->meshMode) {
        for (int j = 0; j < m->rows; ++j)
            for (int i = 0; i < m->cols; ++i) addHandle(Handle{1, i, j}, 3.5f);
    } else {
        for (int i = 0; i < 4; ++i) addHandle(Handle{0, i, 0}, 6.0f);
    }
    m_draw.drawTriangles(shadow, QColor(0, 0, 0, 160));
    m_draw.drawTriangles(handles, kHandle);
    m_draw.drawTriangles(sel, kSelected);

    if (m_rubber) { // selection rectangle
        const QRectF r = QRectF(m_rubberStart, m_rubberEnd).normalized();
        std::vector<float> band;
        pushLine(band, r.topLeft(), r.topRight());
        pushLine(band, r.topRight(), r.bottomRight());
        pushLine(band, r.bottomRight(), r.bottomLeft());
        pushLine(band, r.bottomLeft(), r.topLeft());
        m_draw.drawLines(band, kSelected);
    }
}

bool MappingView::isSelected(const Handle &h) const
{
    return std::find(m_selection.begin(), m_selection.end(), h) != m_selection.end();
}

std::vector<MappingView::Handle> MappingView::allHandles() const
{
    std::vector<Handle> out;
    Mapping *m = mapping();
    if (!m) return out;
    if (m->meshMode) {
        for (int j = 0; j < m->rows; ++j)
            for (int i = 0; i < m->cols; ++i) out.push_back(Handle{1, i, j});
    } else {
        for (int i = 0; i < 4; ++i) out.push_back(Handle{0, i, 0});
    }
    return out;
}

// All selected handles move by the same amount. Positions are read before moving:
// moving a corner changes the homography, hence the position of the other points.
void MappingView::moveSelection(QPointF delta)
{
    std::vector<QPointF> start;
    for (const Handle &h : m_selection) start.push_back(handlePos(h));
    for (size_t k = 0; k < m_selection.size(); ++k) moveHandle(m_selection[k], start[k] + delta);
}

QPointF MappingView::handlePos(const Handle &h) const
{
    Mapping *m = mapping();
    if (!m || !h.valid()) return {};
    if (h.kind == 0) return m->corners[h.i];
    return m->controlPoint(h.i, h.j);
}

void MappingView::moveHandle(const Handle &h, QPointF norm)
{
    Mapping *m = mapping();
    if (!m || !h.valid()) return;
    if (h.kind == 0) m->setCorner(h.i, norm);
    else m->setControlPoint(h.i, h.j, norm);
}

MappingView::Handle MappingView::hitHandle(QPointF p) const
{
    Mapping *m = mapping();
    if (!m) return {};
    Handle best;
    double bestD = 12.0;
    auto test = [&](const Handle &h) {
        const QPointF w = toWidget(outOf(m_layer, handlePos(h)));
        const double d = std::hypot(w.x() - p.x(), w.y() - p.y());
        if (d < bestD) {
            bestD = d;
            best = h;
        }
    };
    if (m->meshMode) {
        for (int j = 0; j < m->rows; ++j)
            for (int i = 0; i < m->cols; ++i) test(Handle{1, i, j});
    } else {
        for (int i = 0; i < 4; ++i) test(Handle{0, i, 0});
    }
    return best;
}

bool MappingView::insideLayer(int index, QPointF p) const
{
    Layer *l = m_engine->layer(index);
    if (!hasPicture(l)) return false;
    auto pts = outline(l->mapping, 16);
    for (QPointF &q : pts) q = outOf(index, q);
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const QPointF a = toWidget(pts[i]), b = toWidget(pts[j]);
        if (((a.y() > p.y()) != (b.y() > p.y())) && (p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()))
            in = !in;
    }
    return in;
}

void MappingView::mousePressEvent(QMouseEvent *e)
{
    // Pan: middle button, or Alt / ⌥ + drag (Space is play / pause)
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier))) {
        m_panning = true;
        m_panLast = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    Engine::Lock lk(&m_engine->mutex());
    const QPointF p = e->position();
    m_lastNorm = toNorm(p);
    const bool locked = m_engine->isLocked(m_layer);
    Handle h = locked ? Handle{} : hitHandle(p);
    const bool additive = e->modifiers() & Qt::ControlModifier; // Ctrl, ⌘ on Mac
    if (h.valid()) {
        if (additive) { // Ctrl/⌘+click: add or remove the point
            const auto it = std::find(m_selection.begin(), m_selection.end(), h);
            if (it != m_selection.end()) m_selection.erase(it);
            else m_selection.push_back(h);
        } else if (!isSelected(h)) {
            m_selection = {h};
        }
        m_primary = h;
        m_dragHandle = isSelected(h); // dragging a selected point moves the whole selection
    } else if (additive && mapping()) { // Ctrl/⌘+drag: selection rectangle
        m_rubber = true;
        m_rubberStart = m_rubberEnd = p;
    } else if (m_layer >= 0 && insideLayer(m_layer, p)) {
        m_selection.clear();
        m_dragLayer = !locked;
    } else {
        // Select the topmost visible layer under the cursor (in a group: its layers before the group itself)
        m_selection.clear();
        std::vector<int> order;
        const int n = m_engine->layerCount();
        for (int i = 0; i < n; ++i) {
            const Layer *l = m_engine->layer(i);
            if (l->parent) continue;
            if (l->isGroup) {
                if (!l->visible) continue;
                for (int k = i + 1; k < n && m_engine->layer(k)->parent == l->id; ++k) order.push_back(k);
            }
            order.push_back(i);
        }
        for (int i : order) {
            if (m_engine->layer(i)->visible && insideLayer(i, p)) {
                const bool pickedLocked = m_engine->isLocked(i);
                lk.unlock();
                emit layerPicked(i);
                lk.relock();
                m_dragLayer = !pickedLocked;
                break;
            }
        }
    }
    if (Mapping *m = mapping()) m_dragBefore = *m;
    update();
}

void MappingView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_panning) {
        m_pan += e->position() - m_panLast;
        m_panLast = e->position();
        update();
        return;
    }
    Engine::Lock lk(&m_engine->mutex());
    const QPointF n = toNorm(e->position());
    // Moves happen in the layer's own canvas (inside its group's mapping)
    QPointF delta = canvasOf(m_layer, n) - canvasOf(m_layer, m_lastNorm);
    m_lastNorm = n;
    if (e->modifiers() & Qt::ShiftModifier) delta *= 0.1; // fine movement
    if (m_rubber) {
        m_rubberEnd = e->position();
    } else if (m_dragHandle) {
        moveSelection(delta);
        emit mappingEdited();
    } else if (m_dragLayer) {
        if (Mapping *m = mapping()) m->translate(delta);
        emit mappingEdited();
    }
    update();
}

void MappingView::mouseReleaseEvent(QMouseEvent *)
{
    if (m_panning) {
        m_panning = false;
        unsetCursor();
        return;
    }
    if (m_rubber) { // adds the points inside the rectangle to the selection
        m_rubber = false;
        Engine::Lock lk(&m_engine->mutex());
        const QRectF r = QRectF(m_rubberStart, m_rubberEnd).normalized();
        for (const Handle &h : allHandles())
            if (r.contains(toWidget(outOf(m_layer, handlePos(h)))) && !isSelected(h)) m_selection.push_back(h);
        update();
        return;
    }
    const bool dragging = m_dragHandle || m_dragLayer;
    const bool handle = m_dragHandle;
    m_dragHandle = m_dragLayer = false;
    if (!dragging || !m_undo) return;
    const Mapping after = cmd::SetMapping::read(m_engine, m_layer);
    if (after.toJson() == m_dragBefore.toJson()) return;
    const QString text = !handle ? QStringLiteral("Move Layer")
                         : m_selection.size() > 1 ? QStringLiteral("Move %1 Points").arg(m_selection.size())
                                                  : QStringLiteral("Move Handle");
    m_undo->push(new cmd::SetMapping(m_engine, m_layer, m_dragBefore, after, text));
}

void MappingView::keyPressEvent(QKeyEvent *e)
{
    Engine::Lock lk(&m_engine->mutex());
    Mapping *m = mapping();
    QPointF d;
    if (e->matches(QKeySequence::SelectAll)) { // Ctrl/⌘+A: every point of the current mode
        m_selection = allHandles();
        update();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Left: d = {-1, 0}; break;
    case Qt::Key_Right: d = {1, 0}; break;
    case Qt::Key_Up: d = {0, -1}; break;
    case Qt::Key_Down: d = {0, 1}; break;
    case Qt::Key_Tab: {
        // Go to the next handle
        if (!m) break;
        if (m->meshMode) {
            int k = m_primary.kind == 1 ? m_primary.j * m->cols + m_primary.i + 1 : 0;
            k %= m->cols * m->rows;
            m_primary = Handle{1, k % m->cols, k / m->cols};
        } else {
            m_primary = Handle{0, m_primary.kind == 0 ? (m_primary.i + 1) % 4 : 0, 0};
        }
        m_selection = {m_primary};
        update();
        return;
    }
    case Qt::Key_Escape:
        m_selection.clear();
        update();
        return;
    default: QOpenGLWidget::keyPressEvent(e); return;
    }
    if (!m || m_engine->isLocked(m_layer)) return;
    const Mapping before = *m;
    // One composition pixel, ×10 with Shift
    const QSize comp = m_engine->compositionSize();
    const double step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    const QPointF delta(d.x() * step / comp.width(), d.y() * step / comp.height());
    if (!m_selection.empty()) {
        moveSelection(delta);
    } else {
        m->translate(delta);
    }
    const Mapping after = *m;
    lk.unlock();
    if (m_undo) m_undo->push(new cmd::SetMapping(m_engine, m_layer, before, after, QStringLiteral("Nudge"), true));
    emit mappingEdited();
    update();
}

bool MappingView::focusNextPrevChild(bool) { return false; }
