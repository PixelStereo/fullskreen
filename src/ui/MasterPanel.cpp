#include "MasterPanel.h"
#include "Engine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

MasterPanel::MasterPanel(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(10);
    v->addWidget(buildMaster());
    v->addWidget(buildOutput());
    v->addWidget(buildComposition());
    v->addWidget(buildPublish());
    v->addStretch();
    syncFromEngine();
}

// ---------------------------------------------------------------------------
// Master
// ---------------------------------------------------------------------------

QWidget *MasterPanel::buildMaster()
{
    auto *g = new QGroupBox(QStringLiteral("Master"));
    auto *v = new QVBoxLayout(g);
    auto *row = new QHBoxLayout;
    m_master = new QSlider(Qt::Horizontal);
    m_master->setRange(0, 100);
    m_master->setValue(100);
    m_master->setToolTip(QStringLiteral("Niveau général de la sortie"));
    m_masterLabel = new QLabel(QStringLiteral("100 %"));
    m_masterLabel->setMinimumWidth(48);
    m_masterLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_master, 1);
    row->addWidget(m_masterLabel);
    v->addLayout(row);

    auto *row2 = new QHBoxLayout;
    m_blackout = new QPushButton(QStringLiteral("Noir"));
    m_blackout->setCheckable(true);
    m_blackout->setMinimumHeight(34);
    m_blackout->setToolTip(QStringLiteral("Fondu au noir / retour (Ctrl+B, ⌘B sur Mac)"));
    m_blackout->setStyleSheet("QPushButton { font-weight:bold; } QPushButton:checked { background:#b3261e; color:white; }");
    m_fade = new QDoubleSpinBox;
    m_fade->setRange(0.0, 30.0);
    m_fade->setSingleStep(0.5);
    m_fade->setDecimals(1);
    m_fade->setSuffix(QStringLiteral(" s"));
    m_fade->setValue(QSettings().value("master/fade", 1.0).toDouble());
    m_fade->setToolTip(QStringLiteral("Durée du fondu au noir et du retour"));
    row2->addWidget(m_blackout, 1);
    row2->addWidget(new QLabel(QStringLiteral("Fondu")));
    row2->addWidget(m_fade);
    v->addLayout(row2);

    connect(m_master, &QSlider::valueChanged, this, [this](int val) {
        // Pendant un noir, le fader règle le niveau de retour sans rallumer.
        if (!m_blackout->isChecked()) m_engine->fadeMaster(val / 100.0, 0.05);
    });
    connect(m_blackout, &QPushButton::toggled, this, [this](bool on) {
        m_engine->fadeMaster(on ? 0.0 : masterValue(), fadeTime());
        emit blackoutChanged(on);
    });
    connect(m_fade, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [](double val) { QSettings().setValue("master/fade", val); });
    return g;
}

void MasterPanel::setBlackout(bool on)
{
    if (m_blackout->isChecked() == on) {
        // Rejoue le fondu (par exemple après une reprise où le niveau a été forcé)
        m_engine->fadeMaster(on ? 0.0 : masterValue(), fadeTime());
        return;
    }
    m_blackout->setChecked(on); // déclenche le fondu et blackoutChanged
}

bool MasterPanel::isBlackout() const { return m_blackout->isChecked(); }
double MasterPanel::masterValue() const { return m_master->value() / 100.0; }
double MasterPanel::fadeTime() const { return m_fade->value(); }

// ---------------------------------------------------------------------------
// Sortie vidéo
// ---------------------------------------------------------------------------

QWidget *MasterPanel::buildOutput()
{
    auto *g = new QGroupBox(QStringLiteral("Sortie vidéo"));
    auto *v = new QVBoxLayout(g);
    auto *form = new QFormLayout;
    m_screens = new QComboBox;
    form->addRow(QStringLiteral("Écran"), m_screens);
    v->addLayout(form);
    auto *row = new QHBoxLayout;
    m_full = new QPushButton(QStringLiteral("Plein écran"));
    m_full->setCheckable(true);
    m_full->setToolTip(QStringLiteral("Ctrl+F (⌘F sur Mac) pour entrer et sortir du plein écran"));
    m_windowed = new QPushButton(QStringLiteral("Fenêtre"));
    m_windowed->setCheckable(true);
    m_windowed->setToolTip(QStringLiteral("Ctrl+Maj+F (⌘⇧F sur Mac)"));
    m_hide = new QPushButton(QStringLiteral("Masquer"));
    m_hide->setCheckable(true);
    row->addWidget(m_full);
    row->addWidget(m_windowed);
    row->addWidget(m_hide);
    v->addLayout(row);
    v->addWidget(note(QStringLiteral("⌘F / Ctrl+F : plein écran sur l'écran choisi (le second écran s'il est branché, "
                                     "sinon l'écran principal), et retour. Fonctionne aussi depuis la sortie.")));

    connect(m_screens, qOverload<int>(&QComboBox::activated), this,
            [this](int i) { emit screenChosen(m_screens->itemData(i).toString()); });
    connect(m_full, &QPushButton::clicked, this, [this] { emit fullscreenRequested(); });
    connect(m_windowed, &QPushButton::clicked, this, [this] { emit windowedRequested(); });
    connect(m_hide, &QPushButton::clicked, this, [this] { emit hideRequested(); });
    return g;
}

void MasterPanel::setScreens(const QList<QPair<QString, QString>> &screens, const QString &current)
{
    QSignalBlocker b(m_screens);
    m_screens->clear();
    for (const auto &s : screens) m_screens->addItem(s.first, s.second);
    m_screens->setCurrentIndex(qMax(0, m_screens->findData(current)));
}

void MasterPanel::setOutputMode(int mode)
{
    m_hide->setChecked(mode == 0);
    m_windowed->setChecked(mode == 1);
    m_full->setChecked(mode == 2);
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

QWidget *MasterPanel::buildComposition()
{
    auto *g = new QGroupBox(QStringLiteral("Composition"));
    auto *form = new QFormLayout(g);
    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("Personnalisée"));
    const QList<QSize> sizes = {{1920, 1080}, {1280, 720}, {3840, 2160}, {4096, 2160}, {1920, 1200}, {2560, 1600},
                                {1400, 1050}, {1024, 768}, {3840, 1080}, {5760, 1080}};
    for (const QSize &s : sizes) m_preset->addItem(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()), s);
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (auto *sb : {m_width, m_height}) {
        sb->setRange(16, 16384);
        sb->setKeyboardTracking(false);
    }
    auto *size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("×")));
    size->addWidget(m_height);
    auto *fit = new QPushButton(QStringLiteral("= écran de sortie"));
    form->addRow(QStringLiteral("Préréglage"), m_preset);
    form->addRow(QStringLiteral("Taille"), size);
    form->addRow(QString(), fit);
    form->addRow(note(QStringLiteral("Réglez la composition sur la résolution native du vidéoprojecteur. "
                                     "Le mapping est relatif : il suit le changement de taille.")));

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
    connect(fit, &QPushButton::clicked, this, &MasterPanel::fitCompositionToScreenRequested);
    return g;
}

void MasterPanel::applyComposition()
{
    if (m_syncing) return;
    const QSize s(m_width->value(), m_height->value());
    if (s == m_engine->compositionSize()) return;
    m_engine->setCompositionSize(s);
    emit compositionEdited();
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

QWidget *MasterPanel::buildPublish()
{
    auto *g = new QGroupBox(QStringLiteral("Publication de la sortie"));
    auto *v = new QVBoxLayout(g);
    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    const QString tips[kPublishKindCount] = {
        QStringLiteral("NDI : réseau. Nécessite NDI Tools ou le NDI Runtime installé sur la machine."),
        QStringLiteral("OMT (Open Media Transport) : réseau, libre et ouvert. Nécessite libomt et libvmx."),
        QStringLiteral("Syphon : partage d'image avec les applications du même Mac (MadMapper, Resolume, OBS…)."),
        QStringLiteral("Spout : partage d'image avec les applications du même PC (Resolume, TouchDesigner, OBS…)."),
    };
    for (int k = 0; k < kPublishKindCount; ++k) {
        m_pubEnabled[k] = new QCheckBox(publishKindName(PublishKind(k)));
        m_pubEnabled[k]->setToolTip(tips[k]);
        m_pubName[k] = new QLineEdit;
        m_pubName[k]->setPlaceholderText(QStringLiteral("Nom de la source"));
        m_pubName[k]->setToolTip(QStringLiteral("Nom sous lequel la sortie apparaît chez les récepteurs"));
        m_pubState[k] = new QLabel;
        m_pubState[k]->setWordWrap(true);
        m_pubState[k]->setStyleSheet("font-size:11px;");
        grid->addWidget(m_pubEnabled[k], k * 2, 0);
        grid->addWidget(m_pubName[k], k * 2, 1);
        grid->addWidget(m_pubState[k], k * 2 + 1, 0, 1, 2);
        if (!publishCompiledIn(PublishKind(k))) {
            m_pubEnabled[k]->setEnabled(false);
            m_pubName[k]->setEnabled(false);
        }
        connect(m_pubEnabled[k], &QCheckBox::toggled, this, &MasterPanel::applyPublish);
        connect(m_pubName[k], &QLineEdit::editingFinished, this, &MasterPanel::applyPublish);
    }
    v->addLayout(grid);

    auto *form = new QFormLayout;
    m_omtQuality = new QComboBox;
    m_omtQuality->addItem(QStringLiteral("Automatique"), 0);
    m_omtQuality->addItem(QStringLiteral("Basse"), 1);
    m_omtQuality->addItem(QStringLiteral("Moyenne"), 50);
    m_omtQuality->addItem(QStringLiteral("Haute"), 100);
    form->addRow(QStringLiteral("Qualité OMT"), m_omtQuality);
    auto *lib = new QHBoxLayout;
    m_libFolder = new QLineEdit;
    m_libFolder->setPlaceholderText(QStringLiteral("emplacements standards"));
    m_libFolder->setToolTip(QStringLiteral("Dossier supplémentaire où chercher les bibliothèques NDI (libndi) et OMT (libomt, libvmx)"));
    auto *browse = new QPushButton(QStringLiteral("…"));
    browse->setFixedWidth(30);
    lib->addWidget(m_libFolder, 1);
    lib->addWidget(browse);
    form->addRow(QStringLiteral("Bibliothèques"), lib);
    v->addLayout(form);
    m_libInfo = note(QString());
    v->addWidget(m_libInfo);
    v->addWidget(note(QStringLiteral("NDI et OMT envoient l'image à la résolution de la composition ; "
                                     "Syphon et Spout la partagent directement sur la carte graphique.")));

    connect(m_omtQuality, qOverload<int>(&QComboBox::activated), this, &MasterPanel::applyPublish);
    connect(m_libFolder, &QLineEdit::editingFinished, this, &MasterPanel::applyPublish);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Dossier des bibliothèques NDI / OMT"),
                                                            m_libFolder->text());
        if (d.isEmpty()) return;
        m_libFolder->setText(d);
        applyPublish();
    });
    return g;
}

void MasterPanel::applyPublish()
{
    if (m_syncing) return;
    PublishSettings s = m_engine->publishSettings();
    for (int k = 0; k < kPublishKindCount; ++k) {
        s.targets[k].enabled = m_pubEnabled[k]->isChecked();
        const QString n = m_pubName[k]->text().trimmed();
        s.targets[k].name = n.isEmpty() ? QStringLiteral("Lanterne") : n;
    }
    s.omtQuality = m_omtQuality->currentData().toInt();
    s.libraryFolder = m_libFolder->text().trimmed();
    if (s == m_engine->publishSettings()) return;
    m_engine->setPublishSettings(s);
    emit publishEdited();
    refreshStatus();
}

void MasterPanel::syncFromEngine()
{
    m_syncing = true;
    const QSize c = m_engine->compositionSize();
    m_width->setValue(c.width());
    m_height->setValue(c.height());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(c)));
    const PublishSettings s = m_engine->publishSettings();
    for (int k = 0; k < kPublishKindCount; ++k) {
        m_pubEnabled[k]->setChecked(s.targets[k].enabled);
        m_pubName[k]->setText(s.targets[k].name);
    }
    m_omtQuality->setCurrentIndex(qMax(0, m_omtQuality->findData(s.omtQuality)));
    m_libFolder->setText(s.libraryFolder);
    m_syncing = false;
    refreshStatus();
}

void MasterPanel::refreshStatus()
{
    const int pct = int(std::lround(m_engine->masterLevel() * 100));
    m_masterLabel->setText(QStringLiteral("%1 %").arg(pct));
    m_masterLabel->setStyleSheet(pct == 0 ? "color:#ff5a4f; font-weight:bold;" : "");

    const QSize c = m_engine->compositionSize();
    if (!m_width->hasFocus() && !m_height->hasFocus() && (c.width() != m_width->value() || c.height() != m_height->value())) {
        m_syncing = true;
        m_width->setValue(c.width());
        m_height->setValue(c.height());
        m_preset->setCurrentIndex(qMax(0, m_preset->findData(c)));
        m_syncing = false;
    }

    for (int k = 0; k < kPublishKindCount; ++k) {
        const PublishState st = m_engine->publishState(PublishKind(k));
        QString text = st.text, color = "#888";
        if (!publishCompiledIn(PublishKind(k))) {
            text = k == int(PublishKind::Syphon) ? QStringLiteral("macOS uniquement") : QStringLiteral("Windows uniquement");
        } else if (st.level == PublishState::Ok) {
            color = "#5fd47a";
            if (st.receivers > 0) text += k == int(PublishKind::Syphon) ? QStringLiteral(" — client connecté")
                                                                        : QStringLiteral(" — %1 récepteur(s)").arg(st.receivers);
            else if (st.receivers == 0) text += QStringLiteral(" — aucun récepteur");
        } else if (st.level == PublishState::Error) {
            color = "#ff6e5f";
        }
        if (text.isEmpty()) text = QStringLiteral("Désactivé");
        m_pubState[k]->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, text.toHtmlEscaped()));
    }
    // Recherche des bibliothèques : environ toutes les deux secondes (accès disque)
    const QString folder = m_libFolder->text().trimmed();
    if (m_libTick++ % 20 != 0 && folder == m_libCheckedFolder) return;
    m_libCheckedFolder = folder;
    const QString ndi = ndiLibraryPath(folder), omt = omtLibraryPath(folder);
    m_libInfo->setText(QStringLiteral("NDI : %1<br>OMT : %2")
                           .arg(ndi.isEmpty() ? QStringLiteral("non trouvé") : ndi.toHtmlEscaped(),
                                omt.isEmpty() ? QStringLiteral("non trouvé") : omt.toHtmlEscaped()));
}
