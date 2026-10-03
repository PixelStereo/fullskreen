#pragma once
#include "IsfLibrary.h"
#include "Layer.h"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

// "Settings" tab, next to Layer and Master: application preferences, saved on this machine (not in the project)
// and applied as soon as they change.
class SettingsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget *parent = nullptr);

    // Mode given to a video or a sound when it is loaded into a layer (Loop unless changed)
    static PlayMode defaultPlayMode();
    // Color models shown by the Color tab of the layers (ColorEditor::Model bits, RGB by default)
    static int colorModels();
    // OSC control and OSCQuery publication (defaults of libossia / score: OSC 1234, OSCQuery 5678)
    static bool oscEnabled();
    static int oscPort();      // UDP, OSC messages
    static int oscQueryPort(); // TCP, OSCQuery (HTTP + WebSocket)

    void setOscStatus(const QString &s);
    // Transition used when a memory gives a layer another source, for the layers that do not choose one:
    // the path saved on this machine (Crossfade by default), found again by its file name in the library
    static QString defaultTransition(const IsfLibrary &library);
    void setTransitions(const QVector<IsfEntry> &transitions, const QString &current); // the library's

signals:
    void playModeChanged();
    void colorModelsChanged();
    void oscChanged();
    void transitionChanged(const QString &path);

private:
    QComboBox *m_playMode = nullptr, *m_colorModels = nullptr, *m_transition = nullptr;
    QCheckBox *m_osc = nullptr;
    QSpinBox *m_oscPort = nullptr, *m_queryPort = nullptr;
    QLabel *m_oscStatus = nullptr;
};
