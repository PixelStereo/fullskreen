#pragma once
#include "GlDraw.h"
#include "Mapping.h"
#include <QOpenGLWidget>
#include <QPointF>
#include <QRectF>
#include <vector>

class Engine;
class QUndoStack;
class QLabel;
class QToolButton;

// Output preview + interactive mapping editing for the selected layer.
// Zoom (wheel, pinch, −/+/Fit buttons) gives finer control when moving the layer or its points:
// moves are computed in composition coordinates, so a higher zoom means smaller steps.
class MappingView : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit MappingView(Engine *engine, QWidget *parent = nullptr);
    ~MappingView() override;

    void setLayer(int index);
    void setShowAllOutlines(bool on) { m_showAll = on; update(); }
    void setUndoStack(QUndoStack *s) { m_undo = s; }
    void zoomBy(double factor);           // around the center of the view
    void zoomToFit();
    double zoom() const { return m_zoom; }

signals:
    void layerPicked(int index);
    void mappingEdited();

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;
    bool focusNextPrevChild(bool next) override; // Tab cycles through the handles

private:
    struct Handle {
        int kind = -1; // 0 = corner, 1 = mesh point
        int i = 0, j = 0;
        bool valid() const { return kind >= 0; }
        bool operator==(const Handle &o) const { return kind == o.kind && i == o.i && j == o.j; }
    };

    Mapping *mapping() const;
    QRectF fitRect() const;  // composition fitted in the widget (zoom 1)
    QRectF viewRect() const; // with zoom and pan
    void zoomAt(QPointF widgetPos, double factor);
    void updateZoomLabel();
    QPointF toWidget(QPointF norm) const;
    QPointF toNorm(QPointF widgetPos) const;
    QPointF handlePos(const Handle &h) const;
    void moveHandle(const Handle &h, QPointF norm);
    Handle hitHandle(QPointF widgetPos) const;
    bool insideLayer(int index, QPointF widgetPos) const;
    std::vector<QPointF> outline(const Mapping &m, int steps) const;
    void pushLine(std::vector<float> &v, QPointF a, QPointF b) const;
    void pushRect(std::vector<float> &v, QPointF c, float half) const;

    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    Mapping m_dragBefore; // state at the start of the gesture, for undo
    GlDraw m_draw;
    int m_layer = -1;
    bool m_showAll = true;

    // Selection: one or several handles (Ctrl/⌘+click, Ctrl/⌘+drag a rectangle, Ctrl/⌘+A); moved together
    std::vector<Handle> m_selection;
    Handle m_primary; // last handle clicked (Tab goes on from it)
    bool isSelected(const Handle &h) const;
    std::vector<Handle> allHandles() const;
    void moveSelection(QPointF delta);
    bool m_dragHandle = false, m_dragLayer = false, m_rubber = false;
    QPointF m_lastNorm, m_rubberStart, m_rubberEnd;

    double m_zoom = 1.0;
    QPointF m_pan;                // offset of the view center, widget pixels
    bool m_panning = false;
    QPointF m_panLast;
    QWidget *m_zoomBar = nullptr;
    QLabel *m_zoomLabel = nullptr;
};
