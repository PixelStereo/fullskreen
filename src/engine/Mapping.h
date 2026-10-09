#pragma once
// Layer mapping: 4 corners (homography, perspective-correct) + warp mesh
// (Catmull-Rom interpolated offsets). Normalized output coordinates: (0,0) top-left, (1,1) bottom-right.

#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <vector>
struct Homography {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0, g = 0, h = 0;
    static Homography squareToQuad(const QPointF q[4]); // q: TL, TR, BR, BL
    QPointF map(double u, double v) const;
};

// Soft edge (like MadMapper's): the picture fades to transparent towards each side, to blend overlapping
// projections. Widths are fractions of the layer's own width (left, right) or height (top, bottom);
// power bends the fade (1 linear; above 1 darker towards the edge, below 1 lighter).
struct SoftEdge {
    enum Side { Left, Right, Top, Bottom };
    bool enabled = false;
    double width[4] = {0.1, 0.1, 0.1, 0.1}; // 0 to 0.5
    double power[4] = {1, 1, 1, 1};         // 0.1 to 8
    bool active() const { return enabled && (width[0] > 0 || width[1] > 0 || width[2] > 0 || width[3] > 0); }
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);
    bool operator==(const SoftEdge &o) const;
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
    SoftEdge soft;                                         // fades the picture towards its sides

    void resetMesh(int c, int r);
    void resetCorners();
    void setCorner(int i, QPointF p);
    void translate(QPointF delta);

    // Bounding box of the mapped shape (corners and mesh points), normalized
    QRectF bounds() const;
    // Moves / scales the whole shape so that its bounding box becomes `to` (corners and mesh warp alike:
    // an axis-aligned scale + translation composes exactly with the homography).
    void setBounds(const QRectF &to);

    // Angle of the top edge, in degrees, measured in pixels of a composition of size `comp`
    double angle(QSize comp) const;
    // Turns the whole shape (corners and mesh) around the middle of its bounds, in pixels of `comp`
    void rotate(double degrees, QSize comp);

    // An upright-or-turned rectangle (a viewport's region): center and size in pixels of `comp`, angle in degrees
    struct Rect {
        QPointF center;
        double w = 1, h = 1, angle = 0;
    };
    Rect rect(QSize comp) const;
    void setRect(const Rect &r, QSize comp); // corners only: no mesh warp

    // Final position (homography + warp) for (u,v) in [0,1].
    QPointF map(double u, double v) const;
    // Inverse of map (Newton iterations from `guess`): (u,v) whose image is `p`
    QPointF unmap(QPointF p, QPointF guess) const;
    bool isIdentity() const; // full frame, no warp

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
