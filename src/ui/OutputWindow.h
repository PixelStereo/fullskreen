#pragma once
#include <QWindow>

class Engine;
class QScreen;

// Output window of one viewport, for its screen or projector. It draws nothing itself:
// the engine's render thread presents the viewport's picture into it, locked to vsync,
// independently of the UI.
class OutputWindow : public QWindow
{
    Q_OBJECT
public:
    OutputWindow(Engine *engine, quint64 viewport);
    quint64 viewport() const { return m_viewport; }
    ~OutputWindow() override;

    // Fullscreen (its own Space on macOS) on the chosen screen, or a window.
    // On macOS, leaving fullscreen is animated: the window is only hidden or moved once the Space has closed.
    void showOn(QScreen *screen, bool fullscreen);
    void hideOutput();
    // Lets go of the viewport at once and deletes the window once it is off screen (after the Space closed)
    void dispose();

    // The screen of that name; without one (or once unplugged), the first screen that is not the main
    // screen (a projector), otherwise the main screen.
    static QScreen *screenNamed(const QString &name);

signals:
    void closeRequested();
    void keyPressed(int key, Qt::KeyboardModifiers modifiers); // forwarded to the main window

protected:
    void exposeEvent(QExposeEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;

private:
    void sync();
    void showOnNow(QScreen *screen, bool fullscreen); // once no Space is left to close
    void hideNow();
    Engine *m_engine;
    quint64 m_viewport = 0;
    bool m_lastExposed = false;
    QSize m_lastSize;
};
