#pragma once
#include "Publish.h"
#include <QWidget>

class Engine;
class QSlider;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QComboBox;
class QCheckBox;
class QLineEdit;

// Onglet « Master » : niveau général et noir, composition, sortie vidéo, publication (NDI, OMT, Syphon, Spout).
class MasterPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MasterPanel(Engine *engine, QWidget *parent = nullptr);

    void setBlackout(bool on);
    bool isBlackout() const;
    double masterValue() const; // niveau choisi au fader (0..1)
    double fadeTime() const;

    // Écrans disponibles (nom affiché, identifiant) et écran choisi
    void setScreens(const QList<QPair<QString, QString>> &screens, const QString &current);
    void setOutputMode(int mode); // 0 masquée, 1 fenêtrée, 2 plein écran
    void syncFromEngine();        // composition, publication (après ouverture d'un projet)
    void refreshStatus();         // niveau, état des publications (appelé périodiquement)

signals:
    void blackoutChanged(bool on);
    void screenChosen(const QString &name);
    void fullscreenRequested();
    void windowedRequested();
    void hideRequested();
    void compositionEdited();
    void publishEdited();
    void fitCompositionToScreenRequested();

private:
    QWidget *buildMaster();
    QWidget *buildComposition();
    QWidget *buildOutput();
    QWidget *buildPublish();
    void applyComposition();
    void applyPublish();

    Engine *m_engine;
    QSlider *m_master = nullptr;
    QLabel *m_masterLabel = nullptr;
    QPushButton *m_blackout = nullptr;
    QDoubleSpinBox *m_fade = nullptr;
    QComboBox *m_preset = nullptr;
    QSpinBox *m_width = nullptr, *m_height = nullptr;
    QComboBox *m_screens = nullptr;
    QPushButton *m_full = nullptr, *m_windowed = nullptr, *m_hide = nullptr;
    QCheckBox *m_pubEnabled[kPublishKindCount] = {};
    QLineEdit *m_pubName[kPublishKindCount] = {};
    QLabel *m_pubState[kPublishKindCount] = {};
    QComboBox *m_omtQuality = nullptr;
    QLineEdit *m_libFolder = nullptr;
    QLabel *m_libInfo = nullptr;
    bool m_syncing = false;
    unsigned m_libTick = 0;
    QString m_libCheckedFolder = QStringLiteral("\x01");
};
