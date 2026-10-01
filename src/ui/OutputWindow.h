#pragma once
#include "GlDraw.h"
#include <QOpenGLWindow>

class Engine;
class QScreen;

// Fenêtre de sortie vers le vidéoprojecteur : affiche la texture de composition plein cadre.
class OutputWindow : public QOpenGLWindow
{
    Q_OBJECT
public:
    explicit OutputWindow(Engine *engine);
    ~OutputWindow() override;

    // Plein écran sur un écran secondaire, fenêtré si c'est l'écran de l'interface.
    void showOn(QScreen *screen, bool fullscreen);

signals:
    void closeRequested();

protected:
    void initializeGL() override;
    void paintGL() override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    Engine *m_engine;
    GlDraw m_draw;
};
