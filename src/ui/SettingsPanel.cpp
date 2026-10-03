#include "SettingsPanel.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

static const char *kPlayModeKey = "playback/defaultMode";
static const char *kColorKey = "ui/colorModel";
static const char *kOscKey = "osc/enabled";
static const char *kOscPortKey = "osc/udpPort";        // (osc/port, osc/queryPort: earlier defaults, ignored)
static const char *kQueryPortKey = "osc/oscQueryPort";
static constexpr int kDefaultOscPort = 1234, kDefaultQueryPort = 5678;

PlayMode SettingsPanel::defaultPlayMode()
{
    return playModeFromKey(QSettings().value(kPlayModeKey).toString(), PlayMode::Loop);
}
int SettingsPanel::colorModels() { return QSettings().value(kColorKey, int(ColorEditor::Rgb)).toInt(); }
bool SettingsPanel::oscEnabled() { return QSettings().value(kOscKey, true).toBool(); }
int SettingsPanel::oscPort() { return QSettings().value(kOscPortKey, kDefaultOscPort).toInt(); }
int SettingsPanel::oscQueryPort() { return QSettings().value(kQueryPortKey, kDefaultQueryPort).toInt(); }

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

SettingsPanel::SettingsPanel(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(10);

    auto *playback = new QGroupBox(QStringLiteral("Playback"));
    auto *form = new QFormLayout(playback);
    m_playMode = new QComboBox;
    for (PlayMode m : {PlayMode::OneShot, PlayMode::Loop, PlayMode::PingPong, PlayMode::Stop})
        m_playMode->addItem(playModeName(m), int(m));
    m_playMode->setCurrentIndex(m_playMode->findData(int(defaultPlayMode())));
    form->addRow(new ResetLabel(QStringLiteral("Default play mode"), [this] { m_playMode->setCurrentIndex(1); }), m_playMode);
    form->addRow(note(QStringLiteral("Given to a video or a sound when it is loaded into a layer. "
                                     "Layers already loaded keep their own mode.")));
    v->addWidget(playback);
    connect(m_playMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(kPlayModeKey, playModeKey(PlayMode(m_playMode->currentData().toInt())));
        emit playModeChanged();
    });

    // Accent color of the interface
    auto *look = new QGroupBox(QStringLiteral("Interface"));
    auto *lookForm = new QFormLayout(look);
    auto *swatch = new QPushButton;
    swatch->setFixedSize(60, 22);
    swatch->setToolTip(QStringLiteral("Color of the selection, the bars and the controls that are on"));
    auto paintSwatch = [swatch] {
        swatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #55555c; border-radius:3px;")
                                  .arg(theme::css()));
    };
    paintSwatch();
    lookForm->addRow(new ResetLabel(QStringLiteral("Accent color"),
                                    [paintSwatch] { theme::setAccent(theme::defaultAccent()); paintSwatch(); }),
                     swatch);
    lookForm->addRow(note(QStringLiteral("Used for the selected layer, the bars, the buttons that are on and the "
                                         "links. A click on the name puts it back to the default light grey.")));
    connect(swatch, &QPushButton::clicked, this, [this, paintSwatch] {
        const QColor c = QColorDialog::getColor(theme::accent(), this, QStringLiteral("Accent color"));
        if (!c.isValid()) return;
        theme::setAccent(c);
        paintSwatch();
    });
    v->addWidget(look);

    auto *color = new QGroupBox(QStringLiteral("Color"));
    auto *cf = new QFormLayout(color);
    m_colorModels = new QComboBox;
    m_colorModels->addItem(QStringLiteral("RGB"), int(ColorEditor::Rgb));
    m_colorModels->addItem(QStringLiteral("HSL"), int(ColorEditor::Hsl));
    m_colorModels->addItem(QStringLiteral("Additive (R G B light)"), int(ColorEditor::Additive));
    m_colorModels->addItem(QStringLiteral("Subtractive (C M Y filters)"), int(ColorEditor::Subtractive));
    m_colorModels->addItem(QStringLiteral("All together"), int(ColorEditor::All));
    m_colorModels->setCurrentIndex(std::max(0, m_colorModels->findData(colorModels())));
    cf->addRow(new ResetLabel(QStringLiteral("Color widgets"), [this] { m_colorModels->setCurrentIndex(0); }), m_colorModels);
    cf->addRow(note(QStringLiteral("How the colors of a new layer's Color tab are edited. "
                                   "Each layer keeps its own choice (Color tab ▸ Edit in).")));
    v->addWidget(color);
    connect(m_colorModels, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(kColorKey, m_colorModels->currentData().toInt());
        emit colorModelsChanged();
    });

    auto *osc = new QGroupBox(QStringLiteral("OSC"));
    auto *of = new QFormLayout(osc);
    m_osc = new QCheckBox(QStringLiteral("Control by OSC and publish the namespace (OSCQuery)"));
    m_osc->setChecked(oscEnabled());
    m_oscPort = new QSpinBox;
    m_queryPort = new QSpinBox;
    for (QSpinBox *sb : {m_oscPort, m_queryPort}) {
        sb->setRange(1024, 65535);
        sb->setKeyboardTracking(false);
    }
    m_oscPort->setValue(oscPort());
    m_queryPort->setValue(oscQueryPort());
    of->addRow(m_osc);
    of->addRow(new ResetLabel(QStringLiteral("OSC port (UDP)"), [this] { m_oscPort->setValue(kDefaultOscPort); }), m_oscPort);
    of->addRow(new ResetLabel(QStringLiteral("OSCQuery port"), [this] { m_queryPort->setValue(kDefaultQueryPort); }),
               m_queryPort);
    m_oscStatus = note(QString());
    of->addRow(m_oscStatus);
    of->addRow(note(QStringLiteral(
        "Announced by zeroconf (_oscjson._tcp, _osc._udp): OSCQuery clients (score, Chataigne, Vezér…) find it "
        "by themselves. Addresses: /master/…, /composition/…, /layers/&lt;name&gt;/… "
        "(groups: /layers/&lt;group&gt;/layers/&lt;name&gt;/…). The whole tree: http://&lt;this machine&gt;:&lt;OSCQuery port&gt;/")));
    v->addWidget(osc);
    v->addStretch();
    auto apply = [this] {
        QSettings s;
        s.setValue(kOscKey, m_osc->isChecked());
        s.setValue(kOscPortKey, m_oscPort->value());
        s.setValue(kQueryPortKey, m_queryPort->value());
        emit oscChanged();
    };
    connect(m_osc, &QCheckBox::toggled, this, apply);
    connect(m_oscPort, qOverload<int>(&QSpinBox::valueChanged), this, apply);
    connect(m_queryPort, qOverload<int>(&QSpinBox::valueChanged), this, apply);
}

void SettingsPanel::setOscStatus(const QString &s) { m_oscStatus->setText(s.toHtmlEscaped()); }
