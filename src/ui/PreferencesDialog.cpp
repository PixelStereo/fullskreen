#include "PreferencesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

static const char *kPlayModeKey = "playback/defaultMode";

PlayMode PreferencesDialog::defaultPlayMode()
{
    return playModeFromKey(QSettings().value(kPlayModeKey).toString(), PlayMode::Loop);
}

void PreferencesDialog::setDefaultPlayMode(PlayMode m) { QSettings().setValue(kPlayModeKey, playModeKey(m)); }

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Preferences"));
    auto *v = new QVBoxLayout(this);

    auto *playback = new QGroupBox(QStringLiteral("Playback"));
    auto *form = new QFormLayout(playback);
    m_playMode = new QComboBox;
    for (PlayMode m : {PlayMode::OneShot, PlayMode::Loop, PlayMode::PingPong, PlayMode::Stop})
        m_playMode->addItem(playModeName(m), int(m));
    m_playMode->setCurrentIndex(m_playMode->findData(int(defaultPlayMode())));
    form->addRow(QStringLiteral("Default play mode"), m_playMode);
    auto *note = new QLabel(QStringLiteral("Given to a video or a sound when it is loaded into a layer. "
                                           "Layers already loaded keep their own mode."));
    note->setWordWrap(true);
    note->setStyleSheet("color:#888; font-size:11px;");
    form->addRow(note);
    v->addWidget(playback);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        setDefaultPlayMode(PlayMode(m_playMode->currentData().toInt()));
        accept();
    });
    setMinimumWidth(420);
}
