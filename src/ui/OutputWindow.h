#pragma once
#include <QWindow>

class Engine;
class QScreen;

// Fenêtre de sortie vers le vidéoprojecteur. Elle ne dessine rien elle-même :
// c'est le fil de rendu du moteur qui y présente la composition, calé sur la synchro verticale,
// indépendamment de l'interface.
class OutputWindow : public QWindow
{
    Q_OBJECT
public:
    explicit OutputWindow(Engine *engine);
    ~OutputWindow() override;

    // Plein écran sur un écran secondaire, fenêtré si c'est l'écran de l'interface.
    void showOn(QScreen *screen, bool fullscreen);
    void hideOutput();

signals:
    void closeRequested();
    void keyPressed(int key, Qt::KeyboardModifiers modifiers); // relayé à la fenêtre principale

protected:
    void exposeEvent(QExposeEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;

private:
    void sync();
    Engine *m_engine;
    bool m_lastExposed = false;
    QSize m_lastSize;
};
