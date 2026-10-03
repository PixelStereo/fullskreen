#include "PreferencesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
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

static QString s_oscStatus;
bool PreferencesDialog::oscEnabled() { return QSettings().value("osc/enabled", true).toBool(); }
int PreferencesDialog::oscPort() { return QSettings().value("osc/port", 9000).toInt(); }
int PreferencesDialog::oscQueryPort() { return QSettings().value("osc/queryPort", 9001).toInt(); }
void PreferencesDialog::setOscStatus(const QString &s) { s_oscStatus = s; }

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

    auto *osc = new QGroupBox(QStringLiteral("OSC"));
    auto *of = new QFormLayout(osc);
    m_osc = new QCheckBox(QStringLiteral("Control by OSC and publish the namespace (OSCQuery)"));
    m_osc->setChecked(oscEnabled());
    m_oscPort = new QSpinBox;
    m_queryPort = new QSpinBox;
    for (QSpinBox *sb : {m_oscPort, m_queryPort}) sb->setRange(1024, 65535);
    m_oscPort->setValue(oscPort());
    m_queryPort->setValue(oscQueryPort());
    of->addRow(m_osc);
    of->addRow(QStringLiteral("OSC port (UDP)"), m_oscPort);
    of->addRow(QStringLiteral("OSCQuery port (HTTP, WebSocket)"), m_queryPort);
    auto *oscNote = new QLabel(s_oscStatus + QStringLiteral(
        "<br>Addresses: /master/…, /composition/…, /layers/&lt;name&gt;/… (groups: /layers/&lt;group&gt;/layers/&lt;name&gt;/…). "
        "Open http://&lt;this machine&gt;:&lt;OSCQuery port&gt;/ for the whole tree."));
    oscNote->setWordWrap(true);
    oscNote->setTextFormat(Qt::RichText);
    oscNote->setStyleSheet("color:#888; font-size:11px;");
    of->addRow(oscNote);
    v->addWidget(osc);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        setDefaultPlayMode(PlayMode(m_playMode->currentData().toInt()));
        QSettings st;
        st.setValue("osc/enabled", m_osc->isChecked());
        st.setValue("osc/port", m_oscPort->value());
        st.setValue("osc/queryPort", m_queryPort->value());
        accept();
    });
    setMinimumWidth(420);
}
