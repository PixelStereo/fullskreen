#pragma once
#include <QWindow>

class Engine;
class QScreen;

// Output window of one viewport — what a screen or a projector shows. It draws nothing itself:
// the engine's render thread presents that viewport's picture into it, locked to vsync,
// independently of the UI.
class OutputWindow : public QWindow
{
    Q_OBJECT
public:
    OutputWindow(Engine *engine, quint64 viewportId);
    quint64 viewportId() const { return m_viewportId; }
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
    quint64 m_viewportId = 0;
    bool m_lastExposed = false;
    bool m_borderlessFullscreen = false; // macOS: fullscreen without a separate Space
    QSize m_lastSize;
};
