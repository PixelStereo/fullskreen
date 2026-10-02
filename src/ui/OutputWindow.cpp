#include "OutputWindow.h"
#include "Engine.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QSurfaceFormat>

#ifdef Q_OS_MACOS
#include "MacPresentation.h"
#endif

OutputWindow::OutputWindow(Engine *engine) : m_engine(engine)
{
    setSurfaceType(QSurface::OpenGLSurface);
    setFormat(QSurfaceFormat::defaultFormat());
    setTitle(QStringLiteral("Fulskrin — Output"));
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
#ifdef Q_OS_MACOS
        // On the screen with the menu bar (no external screen, or output on the main screen),
        // native fullscreen would open a new Space: use a borderless window covering the screen instead,
        // with the menu bar and the Dock hidden.
        if (screen == QGuiApplication::primaryScreen()) {
            macSetMenuBarAndDockHidden(true);
            m_borderlessFullscreen = true;
            setFlags(flags() | Qt::FramelessWindowHint);
            setGeometry(screen->geometry());
            setCursor(Qt::BlankCursor);
            show();
            raise();
            requestActivate();
            return;
        }
#endif
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
    // The render thread stops using the window before it is hidden.
    if (m_lastExposed) {
        m_lastExposed = false;
        m_engine->setOutputExposed(false, m_lastSize);
    }
    hide();
#ifdef Q_OS_MACOS
    if (m_borderlessFullscreen) {
        m_borderlessFullscreen = false;
        setFlags(flags() & ~Qt::FramelessWindowHint);
        macSetMenuBarAndDockHidden(false);
    }
#endif
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
        // Closing the window hides the output (handled by the main window)
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
    // Esc does not close the output (safety during a show): Shift+Esc is required.
    if (e->key() == Qt::Key_Escape && (e->modifiers() & Qt::ShiftModifier)) {
        emit closeRequested();
        return;
    }
    emit keyPressed(e->key(), e->modifiers());
    QWindow::keyPressEvent(e);
}
