#include "MappingView.h"
#include "Commands.h"
#include "Engine.h"

#include <QUndoStack>

#include <QKeyEvent>
#include <QMouseEvent>
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
}

MappingView::~MappingView()
{
    makeCurrent();
    m_draw.destroy();
    doneCurrent();
}

void MappingView::setLayer(int index)
{
    if (index != m_layer) m_selected = Handle{};
    m_layer = index;
    update();
}

Mapping *MappingView::mapping() const
{
    Layer *l = m_engine->layer(m_layer);
    return l ? &l->mapping : nullptr;
}

QRectF MappingView::viewRect() const
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
            if (!l->visible) continue;
            auto pts = outline(l->mapping, 16);
            for (size_t k = 0; k < pts.size(); ++k)
                pushLine(others, toWidget(pts[k]), toWidget(pts[(k + 1) % pts.size()]));
        }
        m_draw.drawLines(others, kOutlineOther);
    }

    Mapping *m = mapping();
    if (!m) return;

    // Warp mesh
    if (m->meshMode) {
        std::vector<float> grid;
        const int steps = 24;
        for (int j = 0; j < m->rows; ++j) {
            const double v = double(j) / (m->rows - 1);
            for (int k = 0; k < steps; ++k)
                pushLine(grid, toWidget(m->map(double(k) / steps, v)), toWidget(m->map(double(k + 1) / steps, v)));
        }
        for (int i = 0; i < m->cols; ++i) {
            const double u = double(i) / (m->cols - 1);
            for (int k = 0; k < steps; ++k)
                pushLine(grid, toWidget(m->map(u, double(k) / steps)), toWidget(m->map(u, double(k + 1) / steps)));
        }
        m_draw.drawLines(grid, kGrid);
    }

    std::vector<float> line;
    auto pts = outline(*m, 32);
    for (size_t k = 0; k < pts.size(); ++k) pushLine(line, toWidget(pts[k]), toWidget(pts[(k + 1) % pts.size()]));
    // Faint diagonals in corners mode, to judge the perspective
    if (!m->meshMode) {
        pushLine(line, toWidget(m->corners[0]), toWidget(m->corners[2]));
        pushLine(line, toWidget(m->corners[1]), toWidget(m->corners[3]));
    }
    m_draw.drawLines(line, kOutline);

    std::vector<float> handles, sel, shadow;
    auto addHandle = [&](const Handle &h, float half) {
        const QPointF p = toWidget(handlePos(h));
        pushRect(shadow, p, half + 1.5f);
        pushRect(h == m_selected ? sel : handles, p, half);
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
        const QPointF w = toWidget(handlePos(h));
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
    if (!l) return false;
    auto pts = outline(l->mapping, 16);
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
    Engine::Lock lk(&m_engine->mutex());
    const QPointF p = e->position();
    m_lastNorm = toNorm(p);
    Handle h = hitHandle(p);
    if (h.valid()) {
        m_selected = h;
        m_dragHandle = true;
    } else if (m_layer >= 0 && insideLayer(m_layer, p)) {
        m_selected = Handle{};
        m_dragLayer = true;
    } else {
        // Select the topmost visible layer under the cursor
        m_selected = Handle{};
        for (int i = 0; i < m_engine->layerCount(); ++i) {
            if (m_engine->layer(i)->visible && insideLayer(i, p)) {
                lk.unlock();
                emit layerPicked(i);
                lk.relock();
                m_dragLayer = true;
                break;
            }
        }
    }
    if (Mapping *m = mapping()) m_dragBefore = *m;
    update();
}

void MappingView::mouseMoveEvent(QMouseEvent *e)
{
    Engine::Lock lk(&m_engine->mutex());
    const QPointF n = toNorm(e->position());
    QPointF delta = n - m_lastNorm;
    m_lastNorm = n;
    if (e->modifiers() & Qt::ShiftModifier) delta *= 0.1; // fine movement
    if (m_dragHandle) {
        moveHandle(m_selected, handlePos(m_selected) + delta);
        emit mappingEdited();
    } else if (m_dragLayer) {
        if (Mapping *m = mapping()) m->translate(delta);
        emit mappingEdited();
    }
    update();
}

void MappingView::mouseReleaseEvent(QMouseEvent *)
{
    const bool dragging = m_dragHandle || m_dragLayer;
    const bool handle = m_dragHandle;
    m_dragHandle = m_dragLayer = false;
    if (!dragging || !m_undo) return;
    const Mapping after = cmd::SetMapping::read(m_engine, m_layer);
    if (after.toJson() == m_dragBefore.toJson()) return;
    m_undo->push(new cmd::SetMapping(m_engine, m_layer, m_dragBefore, after,
                                     handle ? QStringLiteral("Move Handle") : QStringLiteral("Move Layer")));
}

void MappingView::keyPressEvent(QKeyEvent *e)
{
    Engine::Lock lk(&m_engine->mutex());
    Mapping *m = mapping();
    QPointF d;
    switch (e->key()) {
    case Qt::Key_Left: d = {-1, 0}; break;
    case Qt::Key_Right: d = {1, 0}; break;
    case Qt::Key_Up: d = {0, -1}; break;
    case Qt::Key_Down: d = {0, 1}; break;
    case Qt::Key_Tab: {
        // Go to the next handle
        if (!m) break;
        if (m->meshMode) {
            int k = m_selected.kind == 1 ? m_selected.j * m->cols + m_selected.i + 1 : 0;
            k %= m->cols * m->rows;
            m_selected = Handle{1, k % m->cols, k / m->cols};
        } else {
            m_selected = Handle{0, m_selected.kind == 0 ? (m_selected.i + 1) % 4 : 0, 0};
        }
        update();
        return;
    }
    case Qt::Key_Escape: m_selected = Handle{}; update(); return;
    default: QOpenGLWidget::keyPressEvent(e); return;
    }
    if (!m) return;
    const Mapping before = *m;
    // One composition pixel, ×10 with Shift
    const QSize comp = m_engine->compositionSize();
    const double step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    const QPointF delta(d.x() * step / comp.width(), d.y() * step / comp.height());
    if (m_selected.valid()) {
        moveHandle(m_selected, handlePos(m_selected) + delta);
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
