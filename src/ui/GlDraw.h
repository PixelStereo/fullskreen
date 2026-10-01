#pragma once
// Dessin OpenGL côté interface (prévisualisation, sortie) : texture plein cadre,
// lignes et triangles de couleur pour les poignées de mapping.

#include <QColor>
#include <QOpenGLExtraFunctions>
#include <vector>

class GlDraw : protected QOpenGLExtraFunctions
{
public:
    void init();    // contexte courant requis
    void destroy(); // contexte courant requis

    void drawTexture(GLuint tex);                                   // remplit le viewport courant
    void drawLines(const std::vector<float> &ndc, const QColor &c); // paires de points (x,y) en NDC
    void drawTriangles(const std::vector<float> &ndc, const QColor &c);

private:
    void drawColor(const std::vector<float> &ndc, const QColor &c, GLenum mode);
    GLuint m_texProg = 0, m_colorProg = 0;
    GLuint m_quadVao = 0, m_quadVbo = 0, m_dynVao = 0, m_dynVbo = 0;
    GLint m_texLoc = -1, m_colorLoc = -1;
    bool m_ok = false;
};
