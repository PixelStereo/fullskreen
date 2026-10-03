#pragma once
#include "Layer.h"
#include <QDialog>

class QComboBox;
class QCheckBox;
class QSpinBox;

// Application preferences (saved on this machine, not in the project).
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

    // Mode given to a video or a sound when it is loaded into a layer (Loop unless changed)
    static PlayMode defaultPlayMode();
    static void setDefaultPlayMode(PlayMode m);

    // OSC control and OSCQuery publication
    static bool oscEnabled();
    static int oscPort();      // UDP, OSC messages
    static int oscQueryPort(); // TCP, OSCQuery (HTTP + WebSocket)
    static void setOscStatus(const QString &s);

private:
    QComboBox *m_playMode = nullptr;
    QCheckBox *m_osc = nullptr;
    QSpinBox *m_oscPort = nullptr, *m_queryPort = nullptr;
};
