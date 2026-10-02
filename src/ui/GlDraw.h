#pragma once
// UI-side OpenGL drawing (preview, output): full-frame texture,
// colored lines and triangles for the mapping handles.

#include <QColor>
#include <QOpenGLExtraFunctions>
#include <vector>

class GlDraw : protected QOpenGLExtraFunctions
{
public:
    void init();    // requires a current context
    void destroy(); // requires a current context

    void drawTexture(GLuint tex);                                   // fills the current viewport
    void drawLines(const std::vector<float> &ndc, const QColor &c); // pairs of (x,y) points in NDC
    void drawTriangles(const std::vector<float> &ndc, const QColor &c);

private:
    void drawColor(const std::vector<float> &ndc, const QColor &c, GLenum mode);
    GLuint m_texProg = 0, m_colorProg = 0;
    GLuint m_quadVao = 0, m_quadVbo = 0, m_dynVao = 0, m_dynVbo = 0;
    GLint m_texLoc = -1, m_colorLoc = -1;
    bool m_ok = false;
};
