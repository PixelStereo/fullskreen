#include "OutputWindow.h"
#include "Engine.h"

#include <QGuiApplication>
#include <QPointer>
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

// Fullscreen takes a Space of its own on macOS, as any other application does. Leaving it is asynchronous
// there — macOS animates the exit and only then closes the Space — so whatever has to happen next
// (hide, or show windowed) waits for it instead of hiding the window mid-animation, which used to
// leave an empty Space behind.
void OutputWindow::showOn(QScreen *screen, bool fullscreen)
{
#ifdef Q_OS_MACOS
    if (macIsFullScreen(this)) {
        QPointer<OutputWindow> self = this;
        QScreen *target = screen;
        macExitFullScreen(this, [self, target, fullscreen] {
            if (self) self->showOnNow(target, fullscreen);
        });
        return;
    }
#endif
    showOnNow(screen, fullscreen);
}

void OutputWindow::showOnNow(QScreen *screen, bool fullscreen)
{
    if (!screen) screen = QGuiApplication::primaryScreen();
    hideNow();
    setScreen(screen);
    if (fullscreen) {
        setGeometry(screen->geometry());
        setCursor(Qt::BlankCursor);
#ifdef Q_OS_MACOS
        // The window must exist and be on screen before macOS can send it to its own Space.
        macPrepareFullScreen(this);
        show();
        raise();
        requestActivate();
        macEnterFullScreen(this);
#else
        showFullScreen();
#endif
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
#ifdef Q_OS_MACOS
    if (macIsFullScreen(this)) {
        // The render thread lets go now; the window is hidden once the Space has closed.
        if (m_lastExposed) {
            m_lastExposed = false;
            m_engine->setOutputExposed(false, m_lastSize);
        }
        QPointer<OutputWindow> self = this;
        macExitFullScreen(this, [self] {
            if (self) self->hideNow();
        });
        return;
    }
#endif
    hideNow();
}

void OutputWindow::hideNow()
{
    // The render thread stops using the window before it is hidden.
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
