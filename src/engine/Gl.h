#pragma once
// Small OpenGL building blocks shared by the whole engine.
// The engine targets OpenGL 3.3 core (macOS provides 4.1 core, Windows/Linux 3.3+).

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QString>

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

inline QOpenGLExtraFunctions *gl() { return QOpenGLContext::currentContext()->extraFunctions(); }

// Compiles and links a program. Returns 0 on failure (log filled in).
GLuint compileProgram(const QString &vs, const QString &fs, QString *log);

// Texture + render framebuffer.
struct RenderTarget {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    bool isFloat = false;
    // (Re)creates if the size or format changes. Returns true if recreated (contents cleared).
    bool ensure(int w, int h, bool isFloat = false);
    void bind() const;
    void clear(float r = 0, float g = 0, float b = 0, float a = 0) const;
    void destroy();
};

// Simple texture fed from the CPU (video, images).
struct Texture2D {
    GLuint tex = 0;
    int w = 0, h = 0;
    void upload(const void *rgba, int w, int h);
    void destroy();
};
