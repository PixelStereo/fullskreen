#include "MasterPanel.h"
#include "SettingsPanel.h"
#include "Widgets.h"
#include "Engine.h"

#include <QCheckBox>
#include <algorithm>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

CompositionPanel::CompositionPanel(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(10);
    v->addWidget(buildCompositionLevel());
    v->addWidget(buildAudio());
    v->addWidget(buildComposition());
    v->addStretch();
    syncFromEngine();
    // Long screen or sound card names must not widen the panel beyond its column (they are elided).
    for (QComboBox *c : findChildren<QComboBox *>()) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(8);
    }
}

// ---------------------------------------------------------------------------
// Composition Level
// ---------------------------------------------------------------------------

QWidget *CompositionPanel::buildCompositionLevel()
{
    auto *g = new QGroupBox(QStringLiteral("Composition Level"));
    auto *v = new QVBoxLayout(g);
    auto *row = new QHBoxLayout;
    m_composition = new SliderField;
    m_composition->setRange(0, 100);
    m_composition->setDecimals(0);
    m_composition->setSuffix(QStringLiteral(" %"));
    m_composition->setSingleStep(1);
    m_composition->setTicks(10);
    m_composition->setSnaps({0, 100});
    m_composition->setValue(100);
    m_composition->setToolTip(QStringLiteral("Composition output level"));
    m_compositionLabel = new QLabel(QStringLiteral("out 100%"));
    m_compositionLabel->setToolTip(QStringLiteral("What goes out now (the fader, and the blackout over it)"));
    m_compositionLabel->setMinimumWidth(64);
    m_compositionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_composition, 1);
    row->addWidget(m_compositionLabel);
    v->addLayout(row);

    auto *row2 = new QHBoxLayout;
    m_blackout = new QPushButton(QStringLiteral("Blackout"));
    m_blackout->setCheckable(true);
    m_blackout->setMinimumHeight(34);
    m_blackout->setToolTip(QStringLiteral("Fade the picture and the sound out / back in (Ctrl+B, ⌘B on Mac)"));
    m_blackout->setStyleSheet("QPushButton { font-weight:bold; } QPushButton:checked { background:#b3261e; color:white; }");
    m_fade = new NumberBox;
    m_fade->setRange(0.0, 30.0);
    m_fade->setSingleStep(0.5);
    m_fade->setDecimals(1);
    m_fade->setSuffix(QStringLiteral(" s"));
    m_fade->setValue(QSettings().value("composition/fade", 1.0).toDouble());
    m_fade->setToolTip(QStringLiteral("Fade duration for blackout and fade back in"));
    row2->addWidget(m_blackout, 1);
    row2->addWidget(new QLabel(QStringLiteral("Fade")));
    row2->addWidget(m_fade);
    v->addLayout(row2);

    // The fader sets the picture's level; the blackout (picture and sound) is applied on top of it.
    connect(m_composition, &SliderField::valueEdited, this, [this](double val) {
        if (!m_syncing) m_engine->fadeMaster(val / 100.0, 0.05);
    });
    connect(m_blackout, &QPushButton::toggled, this, [this](bool on) {
        if (!m_syncing) m_engine->setBlackout(on, fadeTime());
        emit blackoutChanged(on);
    });
    m_engine->setBlackoutFade(m_fade->value());
    connect(m_fade, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val) {
        QSettings().setValue("composition/fade", val);
        m_engine->setBlackoutFade(val);
    });
    return g;
}

void CompositionPanel::setBlackout(bool on)
{
    if (m_blackout->isChecked() == on) {
        // Replay the fade (e.g. after a recovery where the level was forced)
        m_engine->setBlackout(on, fadeTime());
        return;
    }
    m_blackout->setChecked(on); // triggers the fade and blackoutChanged
}

bool CompositionPanel::isBlackout() const { return m_blackout->isChecked(); }
double CompositionPanel::compositionValue() const { return m_composition->value() / 100.0; }
double CompositionPanel::fadeTime() const { return m_fade->value(); }

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

static QString volumeText(int pct)
{
    if (pct <= 0) return QStringLiteral("−∞ dB");
    const double db = 20.0 * std::log10(pct / 100.0);
    return QStringLiteral("%1%2 dB").arg(db > 0.05 ? "+" : "").arg(db, 0, 'f', 1);
}

QWidget *CompositionPanel::buildAudio()
{
    auto *g = new QGroupBox(QStringLiteral("Sound"));
    auto *v = new QVBoxLayout(g);

    auto *volRow = new QHBoxLayout;
    m_audioVolume = new SliderField;
    m_audioVolume->setRange(0, 200);
    m_audioVolume->setDecimals(0);
    m_audioVolume->setSuffix(QStringLiteral(" %"));
    m_audioVolume->setSingleStep(1);
    m_audioVolume->setTicks(8);
    m_audioVolume->setSnaps({0, 100});
    m_audioVolume->setValue(100);
    m_audioVolume->setToolTip(QStringLiteral("Master volume (100% = unity gain)"));
    m_audioVolumeLabel = new QLabel(volumeText(100));
    m_audioVolumeLabel->setMinimumWidth(64);
    m_audioVolumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_audioMute = new FlagBox(QStringLiteral("Mute"));
    volRow->addWidget(m_audioVolume, 1);
    volRow->addWidget(m_audioVolumeLabel);
    volRow->addWidget(m_audioMute);
    v->addLayout(volRow);

    for (int c = 0; c < 2; ++c) {
        m_meter[c] = new QProgressBar;
        m_meter[c]->setRange(0, 600); // -60 dB .. 0 dB, in tenths of a dB
        m_meter[c]->setTextVisible(false);
        m_meter[c]->setFixedHeight(6);
        m_meter[c]->setStyleSheet("QProgressBar { background:#1b1b1d; border:none; }"
                                  "QProgressBar::chunk { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
                                  "stop:0 #3fae5a, stop:0.8 #3fae5a, stop:0.93 #e0b43a, stop:1 #e5483c); }");
        v->addWidget(m_meter[c]);
    }
    v->addWidget(note(QStringLiteral("The sound card is chosen in Settings ▸ Audio.")));
    connect(m_audioVolume, &SliderField::valueEdited, this, [this](double v) {
        const int pct = int(std::lround(v));
        m_audioVolumeLabel->setText(volumeText(pct));
        if (m_syncing) return;
        m_engine->setAudioVolume(pct / 100.0f);
        emit audioEdited();
    });
    connect(m_audioMute, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing) return;
        m_engine->setAudioMuted(on);
        emit audioEdited();
    });
    m_meterTimer.setInterval(33);
    connect(&m_meterTimer, &QTimer::timeout, this, &CompositionPanel::refreshMeters);
    m_meterTimer.start();
    return g;
}

void CompositionPanel::refreshMeters()
{
    if (!isVisible()) return;
    for (int c = 0; c < 2; ++c) {
        const float pk = m_engine->audioOutput().peak(c);
        const double db = pk > 1e-6f ? 20.0 * std::log10(pk) : -60.0;
        m_meter[c]->setValue(int(std::clamp(db + 60.0, 0.0, 60.0) * 10));
    }
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

QWidget *CompositionPanel::buildComposition()
{
    auto *g = new QGroupBox(QStringLiteral("Composition"));
    auto *form = new QFormLayout(g);
    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("Custom"));
    const QList<QSize> sizes = {{1920, 1080}, {1280, 720}, {3840, 2160}, {4096, 2160}, {1920, 1200}, {2560, 1600},
                                {1400, 1050}, {1024, 768}, {3840, 1080}, {5760, 1080}};
    for (const QSize &s : sizes) m_preset->addItem(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()), s);
    m_width = new IntBox;
    m_height = new IntBox;
    for (auto *sb : {m_width, m_height}) {
        sb->setRange(16, 16384);
        sb->setKeyboardTracking(false);
    }
    auto *size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("×")));
    size->addWidget(m_height);
    form->addRow(QStringLiteral("Preset"), m_preset);
    form->addRow(QStringLiteral("Size"), size);
    form->addRow(note(QStringLiteral("The pixel space every layer lives in. Each viewport shows a part of it, "
                                     "placed by its Spatial tab: three 1920 × 1080 projectors side by side "
                                     "make a 5760 × 1080 composition. Mapping is relative: it follows size changes.")));

    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        const QSize s = m_preset->itemData(i).toSize();
        if (!s.isValid()) return;
        m_syncing = true;
        m_width->setValue(s.width());
        m_height->setValue(s.height());
        m_syncing = false;
        applyComposition();
    });
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, [this] { applyComposition(); });
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, [this] { applyComposition(); });
    return g;
}

void CompositionPanel::applyComposition()
{
    if (m_syncing) return;
    const QSize s(m_width->value(), m_height->value());
    if (s == m_engine->compositionSize()) return;
    m_engine->setCompositionSize(s);
    emit compositionEdited();
}

void CompositionPanel::syncFromEngine()
{
    m_syncing = true;
    const QSize c = m_engine->compositionSize();
    m_width->setValue(c.width());
    m_height->setValue(c.height());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(c)));
    m_audioVolume->setValue(std::lround(m_engine->audioVolume() * 100));
    m_audioVolumeLabel->setText(volumeText(int(m_audioVolume->value())));
    m_audioMute->setChecked(m_engine->audioMuted());
    m_syncing = false;
    refreshStatus();
}

void CompositionPanel::refreshStatus()
{
    // Fader, blackout, fade time and sound follow the changes made elsewhere (OSC)
    m_syncing = true;
    const int fader = int(std::lround(m_engine->masterTarget() * 100));
    if (!m_composition->isDragging() && fader != int(std::lround(m_composition->value()))) m_composition->setValue(fader);
    if (m_blackout->isChecked() != m_engine->blackout()) m_blackout->setChecked(m_engine->blackout());
    if (!m_fade->hasFocus() && std::abs(m_fade->value() - m_engine->blackoutFade()) > 1e-6) m_fade->setValue(m_engine->blackoutFade());
    const int vol = int(std::lround(m_engine->audioVolume() * 100));
    if (!m_audioVolume->isDragging() && vol != int(std::lround(m_audioVolume->value()))) {
        m_audioVolume->setValue(vol);
        m_audioVolumeLabel->setText(volumeText(vol));
    }
    if (m_audioMute->isChecked() != m_engine->audioMuted()) m_audioMute->setChecked(m_engine->audioMuted());
    m_syncing = false;

    const int pct = int(std::lround(m_engine->outputLevel() * 100));
    m_compositionLabel->setText(QStringLiteral("out %1%").arg(pct)); // the fader, and the blackout over it
    m_compositionLabel->setStyleSheet(pct == 0 ? "color:#ff5a4f; font-weight:bold;" : "");

    const QSize c = m_engine->compositionSize();
    if (!m_width->hasFocus() && !m_height->hasFocus() && (c.width() != m_width->value() || c.height() != m_height->value())) {
        m_syncing = true;
        m_width->setValue(c.width());
        m_height->setValue(c.height());
        m_preset->setCurrentIndex(qMax(0, m_preset->findData(c)));
        m_syncing = false;
    }

}
