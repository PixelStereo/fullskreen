#include "SettingsPanel.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QFileInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

static const char *kPlayModeKey = "playback/defaultMode";
static const char *kHardwareKey = "playback/hardwareDecoding";
static const char *kColorKey = "ui/colorModel";
static const char *kOscKey = "osc/enabled";
static const char *kOscPortKey = "osc/udpPort";        // (osc/port, osc/queryPort: earlier defaults, ignored)
static const char *kQueryPortKey = "osc/oscQueryPort";
static const char *kTransitionKey = "memories/transition";
static const char *kRateKey = "render/frameRate";
static const char *kSamplesKey = "render/antialiasing";
static const char *kMipmapsKey = "render/mipmaps";
static const char *kDepthKey = "render/depth";

QList<double> renderChoice::frameRates() { return {0, 24, 25, 30, 50, 60, 120}; }
QString renderChoice::frameRateName(double fps)
{
    return fps <= 0 ? QStringLiteral("Screen refresh") : QStringLiteral("%1 fps").arg(fps);
}
QList<int> renderChoice::depths() { return {8, 10}; }
QString renderChoice::depthName(int bits) { return bits >= 10 ? QStringLiteral("10 bits") : QStringLiteral("8 bits"); }
QList<int> renderChoice::samples() { return {0, 2, 4, 8}; }
QString renderChoice::samplesName(int n) { return n <= 1 ? QStringLiteral("Off") : QStringLiteral("%1× MSAA").arg(n); }

Engine::RenderSettings SettingsPanel::renderDefaults()
{
    QSettings s;
    Engine::RenderSettings r;
    r.frameRate = std::max(0.0, s.value(kRateKey, 0.0).toDouble());
    r.samples = std::max(0, s.value(kSamplesKey, 4).toInt());
    r.mipmaps = s.value(kMipmapsKey, true).toBool() ? 1 : 0;
    r.depth = s.value(kDepthKey, 8).toInt() >= 10 ? 10 : 8;
    return r;
}
static constexpr int kDefaultOscPort = 1234, kDefaultQueryPort = 5678;

PlayMode SettingsPanel::defaultPlayMode()
{
    return playModeFromKey(QSettings().value(kPlayModeKey).toString(), PlayMode::Loop);
}
bool SettingsPanel::hardwareDecoding() { return QSettings().value(kHardwareKey, true).toBool(); }
int SettingsPanel::colorModels() { return QSettings().value(kColorKey, int(ColorEditor::Rgb)).toInt(); }
bool SettingsPanel::oscEnabled() { return QSettings().value(kOscKey, true).toBool(); }
int SettingsPanel::oscPort() { return QSettings().value(kOscPortKey, kDefaultOscPort).toInt(); }
int SettingsPanel::oscQueryPort() { return QSettings().value(kQueryPortKey, kDefaultQueryPort).toInt(); }

QString SettingsPanel::defaultTransition(const IsfLibrary &library)
{
    const QString saved = QSettings().value(kTransitionKey, QStringLiteral("Crossfade.fs")).toString();
    if (QFileInfo::exists(saved)) return saved;
    return library.findByFileName(QFileInfo(saved).fileName()); // empty: a built-in crossfade
}

void SettingsPanel::setTransitions(const QVector<IsfEntry> &transitions, const QString &current)
{
    QSignalBlocker b(m_transition);
    m_transition->clear();
    for (const IsfEntry &t : transitions) {
        m_transition->addItem(t.name, t.path);
        m_transition->setItemData(m_transition->count() - 1, t.description, Qt::ToolTipRole);
    }
    if (m_transition->findData(current) < 0) m_transition->addItem(QStringLiteral("Crossfade (built in)"), QString());
    m_transition->setCurrentIndex(std::max(0, m_transition->findData(current)));
}

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
    m_transition = new QComboBox;
    form->addRow(new ResetLabel(QStringLiteral("Memory transition"),
                                [this] {
                                    const int k = m_transition->findText(QStringLiteral("Crossfade"));
                                    m_transition->setCurrentIndex(std::max(0, k));
                                }),
                 m_transition);
    form->addRow(note(QStringLiteral("When a memory gives a layer another source: the outgoing one keeps playing and "
                                     "this ISF transition takes it to the new one, over the memory's fade. "
                                     "A layer can choose its own (Source tab).")));
    m_hardware = new QCheckBox(QStringLiteral("Hardware decoding"));
    m_hardware->setChecked(hardwareDecoding());
    form->addRow(m_hardware);
    form->addRow(note(QStringLiteral("H.264, HEVC, ProRes… decoded by the graphics hardware when it can (VideoToolbox on "
                                     "macOS, Direct3D 11 on Windows, VA-API on Linux), which leaves the processor to the "
                                     "other videos. HAP never needs it. For the videos loaded from now on.")));
    connect(m_hardware, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(kHardwareKey, on);
        emit hardwareDecodingChanged();
    });
    connect(m_transition, qOverload<int>(&QComboBox::activated), this, [this] {
        const QString path = m_transition->currentData().toString();
        QSettings().setValue(kTransitionKey, path);
        emit transitionChanged(path);
    });
    v->addWidget(playback);
    connect(m_playMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(kPlayModeKey, playModeKey(PlayMode(m_playMode->currentData().toInt())));
        emit playModeChanged();
    });

    // Rendering: the defaults of the projects that do not choose (Master)
    {
        auto *box = new QGroupBox(QStringLiteral("Rendering"));
        auto *rf = new QFormLayout(box);
        const Engine::RenderSettings d = renderDefaults();
        m_rate = new QComboBox;
        for (double r : renderChoice::frameRates()) m_rate->addItem(renderChoice::frameRateName(r), r);
        m_rate->setCurrentIndex(std::max(0, m_rate->findData(d.frameRate)));
        m_samples = new QComboBox;
        for (int n : renderChoice::samples()) m_samples->addItem(renderChoice::samplesName(n), n);
        m_samples->setCurrentIndex(std::max(0, m_samples->findData(d.samples)));
        m_depth = new QComboBox;
        for (int b : renderChoice::depths()) m_depth->addItem(renderChoice::depthName(b), b);
        m_depth->setCurrentIndex(std::max(0, m_depth->findData(d.depth)));
        m_mipmaps = new QCheckBox(QStringLiteral("Smooth pictures drawn smaller (mipmaps)"));
        m_mipmaps->setChecked(d.mipmaps > 0);
        rf->addRow(new ResetLabel(QStringLiteral("Frame rate"), [this] { m_rate->setCurrentIndex(0); }), m_rate);
        rf->addRow(new ResetLabel(QStringLiteral("Antialiasing"), [this] { m_samples->setCurrentIndex(2); }), m_samples);
        rf->addRow(new ResetLabel(QStringLiteral("Color depth"), [this] { m_depth->setCurrentIndex(0); }), m_depth);
        rf->addRow(m_mipmaps);
        rf->addRow(note(QStringLiteral("Defaults of the projects that keep them (Master ▸ Rendering). Screen refresh: "
                                       "the outputs' vertical sync, or the main screen's rate. Antialiasing smooths the "
                                       "edges of the mapped layers; mipmaps the pictures drawn much smaller than they are.")));
        v->addWidget(box);
        auto save = [this] {
            QSettings s;
            s.setValue(kRateKey, m_rate->currentData().toDouble());
            s.setValue(kSamplesKey, m_samples->currentData().toInt());
            s.setValue(kMipmapsKey, m_mipmaps->isChecked());
            s.setValue(kDepthKey, m_depth->currentData().toInt());
            emit renderDefaultsChanged();
        };
        connect(m_rate, qOverload<int>(&QComboBox::activated), this, save);
        connect(m_samples, qOverload<int>(&QComboBox::activated), this, save);
        connect(m_depth, qOverload<int>(&QComboBox::activated), this, save);
        connect(m_mipmaps, &QCheckBox::toggled, this, save);
    }

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
    // Magnetism: its state when Fulskrin starts (the button next to the zoom changes it meanwhile), its reach
    auto *magnetOn = new QCheckBox(QStringLiteral("Magnetism on at start"));
    magnetOn->setChecked(magnet::enabledAtStart());
    auto *reach = new QSpinBox;
    reach->setRange(2, 40);
    reach->setSuffix(QStringLiteral(" px"));
    reach->setValue(magnet::distance());
    lookForm->addRow(magnetOn);
    lookForm->addRow(new ResetLabel(QStringLiteral("Magnet distance"), [reach] { reach->setValue(8); }), reach);
    lookForm->addRow(note(QStringLiteral("Dragged points, layers and viewports are caught by the edges, centers and corners "
                                         "within this distance on screen, and the bars by their notable values. The "
                                         "magnet button next to the zoom turns it on or off; Ctrl / ⌘ held while dragging "
                                         "moves freely.")));
    connect(magnetOn, &QCheckBox::toggled, this, [](bool on) {
        magnet::setEnabledAtStart(on);
        magnet::setEnabled(on);
    });
    connect(reach, qOverload<int>(&QSpinBox::valueChanged), this, [](int px) { magnet::setDistance(px); });
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
