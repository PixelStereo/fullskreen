#pragma once
// Layer mapping: 4 corners (homography, perspective-correct) + warp mesh
// (Catmull-Rom interpolated offsets). Normalized output coordinates: (0,0) top-left, (1,1) bottom-right.

#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <vector>

struct Homography {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0, g = 0, h = 0;
    static Homography squareToQuad(const QPointF q[4]); // q: TL, TR, BR, BL
    QPointF map(double u, double v) const;
};

class Mapping
{
public:
    Mapping() { resetMesh(4, 4); }

    QPointF corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}; // TL, TR, BR, BL
    int cols = 4, rows = 4;                                // number of control points
    std::vector<QPointF> offsets;                         // cols * rows
    bool meshMode = false;                                 // editing mode in the UI
    unsigned revision = 1;                                 // incremented on every change

    void resetMesh(int c, int r);
    void resetCorners();
    void setCorner(int i, QPointF p);
    void translate(QPointF delta);

    // Bounding box of the mapped shape (corners and mesh points), normalized
    QRectF bounds() const;
    // Moves / scales the whole shape so that its bounding box becomes `to` (corners and mesh warp alike:
    // an axis-aligned scale + translation composes exactly with the homography).
    void setBounds(const QRectF &to);

    // Final position (homography + warp) for (u,v) in [0,1].
    QPointF map(double u, double v) const;

    QPointF controlPoint(int i, int j) const;
    void setControlPoint(int i, int j, QPointF p);
    QPointF controlUV(int i, int j) const;

    // Fills (pos.xy in NDC, uv.xy) for a grid of (n+1)^2 vertices.
    void buildVertices(int n, std::vector<float> &out) const;

    // Fits the layer to a source's aspect ratio within a composition (centered).
    void fitAspect(double srcAspect, double compAspect);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);

private:
    QPointF offsetAt(double u, double v) const;
};
