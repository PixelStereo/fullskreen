#pragma once
// Mapping d'un calque : 4 coins (homographie, perspective correcte) + grille de déformation
// (décalages interpolés en Catmull-Rom). Coordonnées normalisées de la sortie : (0,0) haut-gauche, (1,1) bas-droite.

#include <QJsonObject>
#include <QPointF>
#include <vector>

struct Homography {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0, g = 0, h = 0;
    static Homography squareToQuad(const QPointF q[4]); // q : HG, HD, BD, BG
    QPointF map(double u, double v) const;
};

class Mapping
{
public:
    Mapping() { resetMesh(4, 4); }

    QPointF corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}; // HG, HD, BD, BG
    int cols = 4, rows = 4;                                // nombre de points de contrôle
    std::vector<QPointF> offsets;                         // cols * rows
    bool meshMode = false;                                 // mode d'édition dans l'UI
    unsigned revision = 1;                                 // incrémenté à chaque modification

    void resetMesh(int c, int r);
    void resetCorners();
    void setCorner(int i, QPointF p);
    void translate(QPointF delta);

    // Position finale (homographie + déformation) pour (u,v) dans [0,1].
    QPointF map(double u, double v) const;

    QPointF controlPoint(int i, int j) const;
    void setControlPoint(int i, int j, QPointF p);
    QPointF controlUV(int i, int j) const;

    // Remplit (pos.xy en NDC, uv.xy) pour une grille de (n+1)^2 sommets.
    void buildVertices(int n, std::vector<float> &out) const;

    // Fait correspondre le calque au ratio d'une source dans une composition (centré).
    void fitAspect(double srcAspect, double compAspect);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);

private:
    QPointF offsetAt(double u, double v) const;
};
