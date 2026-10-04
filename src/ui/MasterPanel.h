#pragma once
#include "Publish.h"
#include <QTimer>
#include <QWidget>

class Engine;
class SliderField;
class QSlider;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QProgressBar;

// "Master" tab, the project as a whole: master level and blackout (all the viewports at once), audio output,
// and the composition — the pixel space the layers live in and the viewports are placed on.
class MasterPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MasterPanel(Engine *engine, QWidget *parent = nullptr);

    void setBlackout(bool on);
    bool isBlackout() const;
    double masterValue() const; // level set on the fader (0..1)
    double fadeTime() const;

    void syncFromEngine();        // composition, sound (after opening a project)
    void refreshStatus();         // level, composition (called periodically)
    void startAudio();            // opens the audio device saved in the settings (at launch)
    void refreshRenderDefaults(); // the "Default (…)" entries follow the Settings

signals:
    void blackoutChanged(bool on);
    void compositionEdited();
    void audioEdited(); // master volume / mute (saved in the project)

private:
    QWidget *buildMaster();
    QWidget *buildComposition();
    QWidget *buildRendering();
    void syncRendering();
    QWidget *buildAudio();
    void fillAudioDevices();
    void openAudioDevice(const QString &name);
    void refreshMeters();
    void applyComposition();

    Engine *m_engine;
    SliderField *m_master = nullptr;
    QLabel *m_masterLabel = nullptr;
    QPushButton *m_blackout = nullptr;
    QDoubleSpinBox *m_fade = nullptr;
    QComboBox *m_preset = nullptr;
    QSpinBox *m_width = nullptr, *m_height = nullptr;
    QComboBox *m_audioDevice = nullptr;
    SliderField *m_audioVolume = nullptr;
    QLabel *m_audioVolumeLabel = nullptr, *m_audioState = nullptr;
    QCheckBox *m_audioMute = nullptr;
    QProgressBar *m_meter[2] = {};
    QTimer m_meterTimer;
    bool m_syncing = false;
    QComboBox *m_rate = nullptr, *m_samples = nullptr, *m_mipmaps = nullptr;
};
