#include "GlDraw.h"
#include "Gl.h"

void GlDraw::init()
{
    initializeOpenGLFunctions();
    QString log;
    m_texProg = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
        "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
        "void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n",
        &log);
    m_colorProg = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos;\n"
        "void main(){ gl_Position = vec4(a_pos,0.0,1.0); }\n",
        "#version 330 core\nuniform vec4 u_color; out vec4 o;\nvoid main(){ o = u_color; }\n", &log);
    if (!log.isEmpty()) qWarning("GlDraw: %s", qPrintable(log));
    m_texLoc = glGetUniformLocation(m_texProg, "u_tex");
    m_colorLoc = glGetUniformLocation(m_colorProg, "u_color");

    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &m_quadVao);
    glBindVertexArray(m_quadVao);
    glGenBuffers(1, &m_quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenVertexArrays(1, &m_dynVao);
    glBindVertexArray(m_dynVao);
    glGenBuffers(1, &m_dynVbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_dynVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    m_ok = m_texProg && m_colorProg;
}

void GlDraw::destroy()
{
    if (!m_ok) return;
    glDeleteProgram(m_texProg);
    glDeleteProgram(m_colorProg);
    GLuint bufs[] = {m_quadVbo, m_dynVbo};
    glDeleteBuffers(2, bufs);
    GLuint vaos[] = {m_quadVao, m_dynVao};
    glDeleteVertexArrays(2, vaos);
    m_ok = false;
}

void GlDraw::drawTexture(GLuint tex)
{
    if (!m_ok || !tex) return;
    glDisable(GL_BLEND);
    glUseProgram(m_texProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(m_texLoc, 0);
    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void GlDraw::drawColor(const std::vector<float> &ndc, const QColor &c, GLenum mode)
{
    if (!m_ok || ndc.size() < 4) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_colorProg);
    glUniform4f(m_colorLoc, c.redF(), c.greenF(), c.blueF(), c.alphaF());
    glBindVertexArray(m_dynVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_dynVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(ndc.size() * sizeof(float)), ndc.data(), GL_STREAM_DRAW);
    glDrawArrays(mode, 0, GLsizei(ndc.size() / 2));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void GlDraw::drawLines(const std::vector<float> &ndc, const QColor &c) { drawColor(ndc, c, GL_LINES); }
void GlDraw::drawTriangles(const std::vector<float> &ndc, const QColor &c) { drawColor(ndc, c, GL_TRIANGLES); }
