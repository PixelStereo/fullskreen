#include "OutputWindow.h"
#include "Engine.h"

#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>

OutputWindow::OutputWindow(Engine *engine) : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate), m_engine(engine)
{
    setTitle(QStringLiteral("Lanterne — Sortie"));
}

OutputWindow::~OutputWindow()
{
    if (context()) {
        makeCurrent();
        m_draw.destroy();
        doneCurrent();
    }
}

void OutputWindow::showOn(QScreen *screen, bool fullscreen)
{
    if (!screen) screen = QGuiApplication::primaryScreen();
    hide();
    setScreen(screen);
    if (fullscreen) {
        setGeometry(screen->geometry());
        setCursor(Qt::BlankCursor);
        showFullScreen();
    } else {
        const QRect g = screen->availableGeometry();
        const QSize s(qMin(1280, g.width() * 2 / 3), qMin(720, g.height() * 2 / 3));
        setGeometry(QRect(g.center() - QPoint(s.width() / 2, s.height() / 2), s));
        unsetCursor();
        showNormal();
    }
    requestActivate();
}

void OutputWindow::initializeGL() { m_draw.init(); }

void OutputWindow::paintGL()
{
    auto f = context()->extraFunctions();
    const qreal dpr = devicePixelRatio();
    f->glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    m_draw.drawTexture(m_engine->outputTexture());
}

void OutputWindow::keyPressEvent(QKeyEvent *e)
{
    // Échap ne ferme pas la sortie (sécurité en représentation) : il faut Maj+Échap.
    if (e->key() == Qt::Key_Escape && (e->modifiers() & Qt::ShiftModifier)) {
        emit closeRequested();
        return;
    }
    QOpenGLWindow::keyPressEvent(e);
}
