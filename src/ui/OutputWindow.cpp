#include "OutputWindow.h"
#include "Engine.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QSurfaceFormat>

OutputWindow::OutputWindow(Engine *engine) : m_engine(engine)
{
    setSurfaceType(QSurface::OpenGLSurface);
    setFormat(QSurfaceFormat::defaultFormat());
    setTitle(QStringLiteral("Lanterne — Sortie"));
    create();
    m_engine->setOutputWindow(this);
}

OutputWindow::~OutputWindow()
{
    m_engine->setOutputWindow(nullptr);
}

void OutputWindow::sync()
{
    const bool exposed = isVisible() && isExposed();
    const QSize px = size() * devicePixelRatio();
    if (exposed == m_lastExposed && px == m_lastSize) return;
    m_lastExposed = exposed;
    m_lastSize = px;
    m_engine->setOutputExposed(exposed, px);
}

void OutputWindow::showOn(QScreen *screen, bool fullscreen)
{
    if (!screen) screen = QGuiApplication::primaryScreen();
    hideOutput();
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
}

void OutputWindow::hideOutput()
{
    // Le fil de rendu cesse d'utiliser la fenêtre avant qu'elle ne soit masquée.
    if (m_lastExposed) {
        m_lastExposed = false;
        m_engine->setOutputExposed(false, m_lastSize);
    }
    hide();
}

void OutputWindow::exposeEvent(QExposeEvent *e)
{
    QWindow::exposeEvent(e);
    sync();
}

void OutputWindow::resizeEvent(QResizeEvent *e)
{
    QWindow::resizeEvent(e);
    sync();
}

bool OutputWindow::event(QEvent *e)
{
    if (e->type() == QEvent::Close) {
        // Fermer la fenêtre revient à masquer la sortie (géré par la fenêtre principale)
        emit closeRequested();
        e->ignore();
        return true;
    }
    if (e->type() == QEvent::Hide || e->type() == QEvent::Show) {
        const bool r = QWindow::event(e);
        sync();
        return r;
    }
    return QWindow::event(e);
}

void OutputWindow::keyPressEvent(QKeyEvent *e)
{
    // Échap ne ferme pas la sortie (sécurité en représentation) : il faut Maj+Échap.
    if (e->key() == Qt::Key_Escape && (e->modifiers() & Qt::ShiftModifier)) {
        emit closeRequested();
        return;
    }
    emit keyPressed(e->key(), e->modifiers());
    QWindow::keyPressEvent(e);
}
