#pragma once
#include "Engine.h"
#include "IsfLibrary.h"
#include "Layer.h"
#include <QWidget>

class QCheckBox;

// Choices of the rendering settings, shared by Settings (the machine's defaults) and Master (the project's)
namespace renderChoice {
QList<double> frameRates(); // 0: the screen's refresh rate
QString frameRateName(double fps);
QList<int> samples();       // 0: off
QString samplesName(int n);
} // namespace renderChoice
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
    // Videos decoded by the graphics hardware when it can (on unless changed)
    static bool hardwareDecoding();
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
    // Frame rate, antialiasing and mipmaps of the projects that keep the default (Master)
    static Engine::RenderSettings renderDefaults();

signals:
    void playModeChanged();
    void hardwareDecodingChanged();
    void colorModelsChanged();
    void oscChanged();
    void transitionChanged(const QString &path);
    void renderDefaultsChanged();

private:
    QComboBox *m_playMode = nullptr, *m_colorModels = nullptr, *m_transition = nullptr, *m_rate = nullptr,
              *m_samples = nullptr;
    QCheckBox *m_mipmaps = nullptr, *m_hardware = nullptr;
    QCheckBox *m_osc = nullptr;
    QSpinBox *m_oscPort = nullptr, *m_queryPort = nullptr;
    QLabel *m_oscStatus = nullptr;
};
