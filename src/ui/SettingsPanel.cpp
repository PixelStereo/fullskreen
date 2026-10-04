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
#include <QHBoxLayout>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static const char *kPlayModeKey = "playback/defaultMode";
static const char *kHardwareKey = "playback/hardwareDecoding";
static const char *kColorKey = "ui/colorModel";
static const char *kFollowKey = "ui/followFades";
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
static int s_followFades = -1; // read once, then kept up to date by the check box (asked 10 times a second)
bool SettingsPanel::followFades()
{
    if (s_followFades < 0) s_followFades = QSettings().value(kFollowKey, true).toBool() ? 1 : 0;
    return s_followFades == 1;
}
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

SettingsPanel::SettingsPanel(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    root->addWidget(tabs);
    // One page per subject; the page shown is kept from one session to the next
    auto page = [tabs](const QString &title) {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(8, 8, 8, 8);
        l->setSpacing(10);
        tabs->addTab(w, title);
        return l;
    };
    QVBoxLayout *playbackPage = page(QStringLiteral("Playback"));
    QVBoxLayout *renderPage = page(QStringLiteral("Rendering"));
    QVBoxLayout *audioPage = page(QStringLiteral("Audio"));
    QVBoxLayout *interfacePage = page(QStringLiteral("Interface"));
    QVBoxLayout *oscPage = page(QStringLiteral("OSC"));
    tabs->setCurrentIndex(std::clamp(QSettings().value(QStringLiteral("ui/settingsTab"), 0).toInt(), 0, tabs->count() - 1));
    connect(tabs, &QTabWidget::currentChanged, this, [](int i) { QSettings().setValue(QStringLiteral("ui/settingsTab"), i); });

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
    m_hardware = new FlagBox(QStringLiteral("Hardware decoding"));
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
    playbackPage->addWidget(playback);
    connect(m_playMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(kPlayModeKey, playModeKey(PlayMode(m_playMode->currentData().toInt())));
        emit playModeChanged();
    });

    // Rendering: the machine's, for every project
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
        m_mipmaps = new FlagBox(QStringLiteral("Smooth pictures drawn smaller (mipmaps)"));
        m_mipmaps->setChecked(d.mipmaps > 0);
        rf->addRow(new ResetLabel(QStringLiteral("Frame rate"), [this] { m_rate->setCurrentIndex(0); }), m_rate);
        rf->addRow(new ResetLabel(QStringLiteral("Antialiasing"), [this] { m_samples->setCurrentIndex(2); }), m_samples);
        rf->addRow(new ResetLabel(QStringLiteral("Color depth"), [this] { m_depth->setCurrentIndex(0); }), m_depth);
        rf->addRow(m_mipmaps);
        rf->addRow(note(QStringLiteral("For every project opened on this machine. Screen refresh: "
                                       "the outputs' vertical sync, or the main screen's rate. Antialiasing smooths the "
                                       "edges of the mapped layers; mipmaps the pictures drawn much smaller than they are.")));
        renderPage->addWidget(box);
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
    auto *magnetOn = new FlagBox(QStringLiteral("Magnetism on at start"));
    magnetOn->setChecked(magnet::enabledAtStart());
    auto *reach = new IntBox;
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
    auto *follow = new FlagBox(QStringLiteral("Fields follow the values during memory fades"));
    follow->setChecked(followFades());
    lookForm->addRow(follow);
    lookForm->addRow(note(QStringLiteral("On: the inspector's sliders and fields (opacity, viewports, color, soft edge, "
                                         "position, volume, speed, ISF parameters…) move with the values while a memory "
                                         "fades them. Off: they stay still and show the memory's values once the fade is "
                                         "over. A field being edited is never moved.")));
    connect(follow, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(kFollowKey, on);
        s_followFades = on ? 1 : 0;
    });
    connect(swatch, &QPushButton::clicked, this, [this, paintSwatch] {
        const QColor c = QColorDialog::getColor(theme::accent(), this, QStringLiteral("Accent color"));
        if (!c.isValid()) return;
        theme::setAccent(c);
        paintSwatch();
    });
    interfacePage->addWidget(look);

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
    interfacePage->addWidget(color);
    connect(m_colorModels, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(kColorKey, m_colorModels->currentData().toInt());
        emit colorModelsChanged();
    });

    auto *osc = new QGroupBox(QStringLiteral("OSC"));
    auto *of = new QFormLayout(osc);
    m_osc = new FlagBox(QStringLiteral("Control by OSC and publish the namespace (OSCQuery)"));
    m_osc->setChecked(oscEnabled());
    m_oscPort = new IntBox;
    m_queryPort = new IntBox;
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
    oscPage->addWidget(osc);
    audioPage->addWidget(buildAudio());
    for (QVBoxLayout *l : {playbackPage, renderPage, audioPage, interfacePage, oscPage}) l->addStretch();
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

// Audio: the sound card of this machine (the master volume is the composition's)
QWidget *SettingsPanel::buildAudio()
{
    auto *g = new QGroupBox(QStringLiteral("Audio Output"));
    auto *v = new QVBoxLayout(g);
    auto *devRow = new QHBoxLayout;
    m_audioDevice = new QComboBox;
    m_audioDevice->setToolTip(QStringLiteral("Sound card or audio interface used for the sound of every layer"));
    m_audioDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_audioDevice->setMinimumContentsLength(8);
    auto *rescan = new QToolButton;
    rescan->setText(QStringLiteral("⟳"));
    rescan->setToolTip(QStringLiteral("Refresh the list of audio devices"));
    devRow->addWidget(m_audioDevice, 1);
    devRow->addWidget(rescan);
    v->addLayout(devRow);
    m_audioState = note(QString());
    v->addWidget(m_audioState);
    v->addWidget(note(QStringLiteral("Saved on this machine. The master volume and mute are in the Composition tab.")));
    connect(rescan, &QToolButton::clicked, this, [this] { fillAudioDevices(); });
    connect(m_audioDevice, qOverload<int>(&QComboBox::activated), this, [this](int) {
        const QString name = m_audioDevice->currentData().toString();
        QSettings().setValue("audio/device", name);
        openAudioDevice(name);
    });
    return g;
}

void SettingsPanel::fillAudioDevices()
{
    const QString saved = QSettings().value("audio/device").toString();
    QSignalBlocker b(m_audioDevice);
    m_audioDevice->clear();
    m_audioDevice->addItem(QStringLiteral("System default"), QString());
    for (const QString &n : AudioOutput::deviceNames()) m_audioDevice->addItem(n, n);
    int idx = m_audioDevice->findData(saved);
    if (idx < 0 && !saved.isEmpty()) { // saved device currently unplugged: keep it visible
        m_audioDevice->addItem(saved + QStringLiteral(" (not connected)"), saved);
        idx = m_audioDevice->count() - 1;
    }
    m_audioDevice->setCurrentIndex(qMax(0, idx));
}

void SettingsPanel::openAudioDevice(const QString &name)
{
    QString err;
    if (m_engine->startAudio(name, &err)) {
        m_audioState->setText(QStringLiteral("Playing on \"%1\" · %2 kHz · latency %3 ms")
                                  .arg(m_engine->audioOutput().deviceName().toHtmlEscaped())
                                  .arg(AudioOutput::kSampleRate / 1000)
                                  .arg(int(std::lround(m_engine->audioOutput().latency() * 1000))));
        m_audioState->setStyleSheet("color:#888; font-size:11px;");
    } else {
        m_audioState->setText(err.toHtmlEscaped());
        m_audioState->setStyleSheet("color:#ff6b5b; font-size:11px;");
    }
}

void SettingsPanel::startAudio()
{
    fillAudioDevices();
    openAudioDevice(QSettings().value("audio/device").toString());
}

void SettingsPanel::setOscStatus(const QString &s) { m_oscStatus->setText(s.toHtmlEscaped()); }
