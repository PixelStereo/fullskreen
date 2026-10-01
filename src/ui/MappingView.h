#pragma once
#include "GlDraw.h"
#include <QOpenGLWidget>
#include <QPointF>
#include <QRectF>

class Engine;
class Mapping;

// Prévisualisation de la sortie + édition interactive du mapping du calque sélectionné.
class MappingView : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit MappingView(Engine *engine, QWidget *parent = nullptr);
    ~MappingView() override;

    void setLayer(int index);
    void setShowAllOutlines(bool on) { m_showAll = on; update(); }

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
    bool focusNextPrevChild(bool next) override; // Tab sert à parcourir les poignées

private:
    struct Handle {
        int kind = -1; // 0 = coin, 1 = point de grille
        int i = 0, j = 0;
        bool valid() const { return kind >= 0; }
        bool operator==(const Handle &o) const { return kind == o.kind && i == o.i && j == o.j; }
    };

    Mapping *mapping() const;
    QRectF viewRect() const;
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
    GlDraw m_draw;
    int m_layer = -1;
    bool m_showAll = true;

    Handle m_selected;
    bool m_dragHandle = false, m_dragLayer = false;
    QPointF m_lastNorm;
};
