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

// "Composition" tab, the project as a whole: its opacity and blackout (all viewports at once), the sound
// volume, and its size — the pixel space the layers live in and the viewports are placed on. The machine's choices
// (rendering, sound card) are in Settings.
class CompositionPanel : public QWidget
{
    Q_OBJECT
public:
    explicit CompositionPanel(Engine *engine, QWidget *parent = nullptr);

    void setBlackout(bool on);
    bool isBlackout() const;
    double compositionValue() const; // level set on the fader (0..1)
    double fadeTime() const;

    void syncFromEngine();        // composition, sound (after opening a project)
    void refreshStatus();         // level, composition (called periodically)

signals:
    void blackoutChanged(bool on);
    void compositionEdited();
    void audioEdited(); // composition volume (saved in the project)

private:
    QWidget *buildCompositionLevel();
    QWidget *buildComposition();
    QWidget *buildAudio();
    void refreshMeters();
    void applyComposition();
    void syncRate();

    Engine *m_engine;
    SliderField *m_composition = nullptr;
    QLabel *m_compositionLabel = nullptr;
    QPushButton *m_blackout = nullptr;
    QPushButton *m_pause = nullptr;
    SliderField *m_speed = nullptr;
    QDoubleSpinBox *m_fade = nullptr;
    QComboBox *m_preset = nullptr;
    QSpinBox *m_width = nullptr, *m_height = nullptr;
    SliderField *m_audioVolume = nullptr;
    QLabel *m_audioVolumeLabel = nullptr;
    QComboBox *m_rate = nullptr;
    QProgressBar *m_meter[2] = {};
    QTimer m_meterTimer;
    bool m_syncing = false;
};
