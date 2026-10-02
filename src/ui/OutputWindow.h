#pragma once
#include <QWindow>

class Engine;
class QScreen;

// Output window for the projector. It draws nothing itself:
// the engine's render thread presents the composition into it, locked to vsync,
// independently of the UI.
class OutputWindow : public QWindow
{
    Q_OBJECT
public:
    explicit OutputWindow(Engine *engine);
    ~OutputWindow() override;

    // Fullscreen on a secondary screen, windowed if it is the UI screen.
    void showOn(QScreen *screen, bool fullscreen);
    void hideOutput();

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
    Engine *m_engine;
    bool m_lastExposed = false;
    bool m_borderlessFullscreen = false; // macOS: fullscreen without a separate Space
    QSize m_lastSize;
};
