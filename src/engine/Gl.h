#pragma once
// Petites briques OpenGL partagées par tout le moteur.
// Le moteur cible OpenGL 3.3 core (macOS fournit 4.1 core, Windows/Linux 3.3+).

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

// Compile et lie un programme. Retourne 0 en cas d'échec (log rempli).
GLuint compileProgram(const QString &vs, const QString &fs, QString *log);

// Texture + framebuffer de rendu.
struct RenderTarget {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    bool isFloat = false;
    // (Re)crée si la taille ou le format change. Retourne true si recréé (contenu effacé).
    bool ensure(int w, int h, bool isFloat = false);
    void bind() const;
    void clear(float r = 0, float g = 0, float b = 0, float a = 0) const;
    void destroy();
};

// Texture simple alimentée depuis le CPU (vidéo, images).
struct Texture2D {
    GLuint tex = 0;
    int w = 0, h = 0;
    void upload(const void *rgba, int w, int h);
    void destroy();
};
