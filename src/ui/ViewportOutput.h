#pragma once
#include "Publish.h"
#include <QWidget>

class Engine;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

// Output tab of a viewport: its size in pixels, the screen it goes to and how it is shown there (hidden,
// in a window, fullscreen), and what it publishes (NDI, OMT, Syphon, Spout). Each viewport has its own.
class ViewportOutputPanel : public QWidget
{
    Q_OBJECT
public:
    ViewportOutputPanel(Engine *engine, quint64 viewport, QWidget *parent = nullptr);
    void refreshStatus(); // mode set elsewhere (⌘F, the window closed), publishing states

signals:
    void edited();        // saved in the project
    void outputChanged(); // screen or mode: the window follows

private:
    void applySize();
    void applyPublish();
    int index() const;
    Engine *m_engine;
    quint64 m_viewport;
    QComboBox *m_preset = nullptr, *m_screen = nullptr, *m_omtQuality = nullptr;
    QSpinBox *m_width = nullptr, *m_height = nullptr;
    QButtonGroup *m_mode = nullptr;
    QCheckBox *m_pubEnabled[kPublishKindCount] = {};
    QLineEdit *m_pubName[kPublishKindCount] = {};
    QLabel *m_pubState[kPublishKindCount] = {};
    QLineEdit *m_libFolder = nullptr;
    QLabel *m_libInfo = nullptr;
    bool m_syncing = false;
    unsigned m_libTick = 0;
};
