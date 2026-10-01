#include "Gl.h"
#include <QByteArray>
#include <QDebug>

static GLuint compileStage(GLenum type, const QString &src, QString *log)
{
    auto f = gl();
    GLuint s = f->glCreateShader(type);
    QByteArray utf8 = src.toUtf8();
    const char *p = utf8.constData();
    f->glShaderSource(s, 1, &p, nullptr);
    f->glCompileShader(s);
    GLint ok = 0;
    f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        f->glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        QByteArray b(qMax(len, 1) + 1, 0);
        f->glGetShaderInfoLog(s, len, nullptr, b.data());
        if (log)
            *log += (type == GL_VERTEX_SHADER ? QStringLiteral("[vertex] ") : QStringLiteral("[fragment] "))
                    + QString::fromUtf8(b).trimmed() + '\n';
        f->glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint compileProgram(const QString &vs, const QString &fs, QString *log)
{
    auto f = gl();
    GLuint v = compileStage(GL_VERTEX_SHADER, vs, log);
    GLuint fr = compileStage(GL_FRAGMENT_SHADER, fs, log);
    if (!v || !fr) {
        if (v) f->glDeleteShader(v);
        if (fr) f->glDeleteShader(fr);
        return 0;
    }
    GLuint p = f->glCreateProgram();
    f->glAttachShader(p, v);
    f->glAttachShader(p, fr);
    f->glLinkProgram(p);
    f->glDeleteShader(v);
    f->glDeleteShader(fr);
    GLint ok = 0;
    f->glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        f->glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        QByteArray b(qMax(len, 1) + 1, 0);
        f->glGetProgramInfoLog(p, len, nullptr, b.data());
        if (log) *log += QStringLiteral("[link] ") + QString::fromUtf8(b).trimmed() + '\n';
        f->glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool RenderTarget::ensure(int nw, int nh, bool flt)
{
    nw = qMax(1, nw);
    nh = qMax(1, nh);
    if (fbo && nw == w && nh == h && flt == isFloat)
        return false;
    destroy();
    auto f = gl();
    w = nw; h = nh; isFloat = flt;
    f->glGenTextures(1, &tex);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, flt ? GL_RGBA32F : GL_RGBA8, w, h, 0, GL_RGBA,
                    flt ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glGenFramebuffers(1, &fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        qWarning() << "Framebuffer incomplet" << Qt::hex << st << w << h << flt;
    clear();
    return true;
}

void RenderTarget::bind() const
{
    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    f->glViewport(0, 0, w, h);
}

void RenderTarget::clear(float r, float g, float b, float a) const
{
    auto f = gl();
    bind();
    f->glClearColor(r, g, b, a);
    f->glClear(GL_COLOR_BUFFER_BIT);
}

void RenderTarget::destroy()
{
    auto f = gl();
    if (fbo) f->glDeleteFramebuffers(1, &fbo);
    if (tex) f->glDeleteTextures(1, &tex);
    fbo = tex = 0;
    w = h = 0;
}

void Texture2D::upload(const void *rgba, int nw, int nh)
{
    auto f = gl();
    if (!tex) {
        f->glGenTextures(1, &tex);
        f->glBindTexture(GL_TEXTURE_2D, tex);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (nw != w || nh != h) {
        w = nw; h = nh;
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

void Texture2D::destroy()
{
    if (tex) gl()->glDeleteTextures(1, &tex);
    tex = 0;
    w = h = 0;
}
