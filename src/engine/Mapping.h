#pragma once
// Layer mapping: a transform, a corner pin (homography, perspective-correct) and a warp mesh (Catmull-Rom).

#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
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
    bool operator==(const SoftEdge &o) const;
};

// Where a layer's picture goes on the composition (normalized: (0,0) top left, (1,1) bottom right). Stored as values
// that each mean one thing — so that each one can be set, faded and animated on its own:
//  - its transform: the center of its rectangle (position), its size (a fraction of the composition), its rotation
//    (degrees, in pixels: clockwise) about its pivot (a point of the picture, in its own frame 0..1);
//  - its corner pin: how far each corner is pulled from the rectangle (in its own frame: 1 = its width / height),
//    which makes the perspective (a homography);
//  - its warp mesh: control points pulled from where the corner pin puts them (in its own frame, Catmull-Rom).
// The corners and the points on the composition are made from these (corner(), map()). Turning, moving or scaling it
// turns, moves and scales its corner pin and its mesh with it.
class Mapping
{
public:
    Mapping() { resetMesh(4, 4); }

    QPointF position{0.5, 0.5};      // the center of its rectangle
    QSizeF size{1, 1};               // its rectangle, a fraction of the composition
    double rotation = 0;             // degrees about the pivot, clockwise
    QPointF pivot{0.5, 0.5};         // the center of the rotation, in its own frame (0..1 over its picture)
    QPointF pins[4];                 // corner pin: TL, TR, BR, BL pulled from the rectangle, in its own frame
    int cols = 4, rows = 4;          // number of control points
    std::vector<QPointF> offsets;    // cols * rows, in its own frame
    bool meshMode = false;           // editing mode in the UI
    unsigned revision = 1;           // incremented on every change
    SoftEdge soft;                   // fades the picture towards its sides
    double aspect = 16.0 / 9.0;      // the composition's width / height (its rotation is in pixels): set by the engine

    void resetMesh(int c, int r);
    void resetCorners(); // the whole composition, upright, no corner pin
    QPointF corner(int k) const; // TL, TR, BR, BL on the composition
    void setCorner(int i, QPointF p); // by its corner pin
    void translate(QPointF delta);

    // Its own frame (u, v: 0..1 over its rectangle) to the composition, and back
    QPointF toComposition(QPointF q) const;
    QPointF fromComposition(QPointF p) const;

    // Bounding box of the mapped shape (corners and mesh points), normalized
    QRectF bounds() const;
    // Moves and scales it so that its bounding box becomes `to` (exact when it is upright)
    void setBounds(const QRectF &to);

    // The pivot on the composition (normalized), and the other way round: moving it moves no picture
    QPointF pivotPoint() const;
    void setPivotPoint(QPointF p);

    double angle(QSize) const { return rotation; }
    void rotate(double degrees, QSize comp); // about its pivot

    // An upright-or-turned rectangle (a viewport's region): center and size in pixels of `comp`, angle in degrees
    struct Rect {
        QPointF center;
        double w = 1, h = 1, angle = 0;
    };
    Rect rect(QSize comp) const;
    void setRect(const Rect &r, QSize comp); // no corner pin, no mesh warp

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

    // Its mesh in a file (cols, rows, offsets): its other values are its layer's parameters
    QJsonObject meshJson() const;
    void setMeshJson(const QJsonObject &o);
    // The same shape: every stored value equal (its revision and the composition's aspect aside)
    bool operator==(const Mapping &o) const;
    bool operator!=(const Mapping &o) const { return !(*this == o); }

private:
    QPointF offsetAt(double u, double v) const;
    Homography local() const; // the unit square onto its corner-pinned quad, in its own frame
};
