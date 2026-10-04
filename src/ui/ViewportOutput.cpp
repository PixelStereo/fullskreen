#include "ViewportOutput.h"
#include "Engine.h"
#include "OutputWindow.h"
#include "Widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QVBoxLayout>

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

static QSize nativeSize(const QScreen *sc) { return sc->geometry().size() * sc->devicePixelRatio(); }

ViewportOutputPanel::ViewportOutputPanel(Engine *engine, quint64 viewport, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_viewport(viewport)
{
    QSize size;
    QString screen;
    int mode = 0;
    PublishSettings ps = m_engine->publishSettings(viewport);
    {
        Engine::Lock lk(&m_engine->mutex());
        if (const Layer *l = m_engine->layer(index())) {
            size = l->viewportSize();
            screen = l->vpScreen;
            mode = l->vpMode;
        }
    }
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);

    // --- Picture: its size in pixels
    auto *picture = new QGroupBox(QStringLiteral("Picture"));
    auto *pf = new QFormLayout(picture);
    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("Custom"));
    const QList<QSize> sizes = {{1920, 1080}, {1280, 720}, {3840, 2160}, {4096, 2160}, {1920, 1200},
                                {2560, 1600}, {1400, 1050}, {1024, 768}};
    for (const QSize &s : sizes) m_preset->addItem(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()), s);
    m_width = new IntBox;
    m_height = new IntBox;
    for (QSpinBox *sb : {m_width, m_height}) {
        sb->setRange(1, 16384);
        sb->setKeyboardTracking(false);
    }
    m_width->setValue(size.width());
    m_height->setValue(size.height());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(size)));
    auto *row = new QHBoxLayout;
    row->addWidget(m_width);
    row->addWidget(new QLabel(QStringLiteral("×")));
    row->addWidget(m_height);
    auto *fit = new QPushButton(QStringLiteral("= screen"));
    fit->setToolTip(QStringLiteral("The native resolution of the screen chosen below"));
    row->addWidget(fit);
    pf->addRow(QStringLiteral("Preset"), m_preset);
    pf->addRow(QStringLiteral("Size"), row);
    pf->addRow(note(QStringLiteral("Its place in the composition is set in the Spatial tab.")));
    v->addWidget(picture);
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        const QSize s = m_preset->itemData(i).toSize();
        if (!s.isValid()) return;
        m_syncing = true;
        m_width->setValue(s.width());
        m_height->setValue(s.height());
        m_syncing = false;
        applySize();
    });
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, &ViewportOutputPanel::applySize);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, &ViewportOutputPanel::applySize);

    // --- Screen and how it is shown there
    auto *out = new QGroupBox(QStringLiteral("Screen"));
    auto *of = new QFormLayout(out);
    m_screen = new QComboBox;
    m_screen->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_screen->setMinimumContentsLength(8);
    for (QScreen *sc : QGuiApplication::screens()) {
        const QSize px = nativeSize(sc);
        m_screen->addItem(QStringLiteral("%1  (%2 × %3)%4")
                              .arg(sc->name())
                              .arg(px.width())
                              .arg(px.height())
                              .arg(sc == QGuiApplication::primaryScreen() ? QStringLiteral(" — main") : QString()),
                          sc->name());
    }
    // No screen chosen yet (or unplugged): the one the window would go to
    const QScreen *target = OutputWindow::screenNamed(screen);
    m_screen->setCurrentIndex(qMax(0, m_screen->findData(target ? target->name() : screen)));
    of->addRow(QStringLiteral("Screen"), m_screen);
    auto *modes = new QHBoxLayout;
    m_mode = new QButtonGroup(this);
    m_mode->setExclusive(true);
    const char *names[] = {"Hidden", "Window", "Fullscreen"};
    for (int k = 0; k < 3; ++k) {
        auto *b = new QPushButton(QString::fromUtf8(names[k]));
        b->setCheckable(true);
        b->setChecked(mode == k);
        b->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:%2; }")
                             .arg(theme::css(), theme::onAccent().name()));
        m_mode->addButton(b, k);
        modes->addWidget(b);
    }
    of->addRow(QStringLiteral("Show"), modes);
    of->addRow(note(QStringLiteral("⌘F / Ctrl+F puts every viewport fullscreen on its screen at once, "
                                   "and back to how each one was.")));
    v->addWidget(out);
    auto applyOutput = [this] {
        if (m_syncing) return;
        m_engine->setViewportOutput(index(), m_screen->currentData().toString(), m_mode->checkedId());
        emit edited();
        emit outputChanged();
    };
    connect(m_screen, qOverload<int>(&QComboBox::activated), this, applyOutput);
    connect(m_mode, &QButtonGroup::idClicked, this, applyOutput);
    connect(fit, &QPushButton::clicked, this, [this] {
        for (QScreen *sc : QGuiApplication::screens())
            if (sc->name() == m_screen->currentData().toString()) {
                const QSize px = nativeSize(sc);
                m_syncing = true;
                m_width->setValue(px.width());
                m_height->setValue(px.height());
                m_syncing = false;
                applySize();
            }
    });

    // --- Publishing
    auto *pub = new QGroupBox(QStringLiteral("Publishing"));
    auto *pv = new QVBoxLayout(pub);
    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    const QString tips[kPublishKindCount] = {
        QStringLiteral("NDI: network. Requires NDI Tools or the NDI Runtime installed on this machine."),
        QStringLiteral("OMT (Open Media Transport): network, free and open. Requires libomt and libvmx."),
        QStringLiteral("Syphon: shares the picture with apps on the same Mac (MadMapper, Resolume, OBS…)."),
        QStringLiteral("Spout: shares the picture with apps on the same PC (Resolume, TouchDesigner, OBS…)."),
    };
    for (int k = 0; k < kPublishKindCount; ++k) {
        m_pubEnabled[k] = new QCheckBox(publishKindName(PublishKind(k)));
        m_pubEnabled[k]->setToolTip(tips[k]);
        m_pubEnabled[k]->setChecked(ps.targets[k].enabled);
        m_pubName[k] = new QLineEdit(ps.targets[k].name);
        m_pubName[k]->setPlaceholderText(QStringLiteral("Source name"));
        m_pubName[k]->setToolTip(QStringLiteral("Name under which this viewport appears on receivers"));
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
        connect(m_pubEnabled[k], &QCheckBox::toggled, this, &ViewportOutputPanel::applyPublish);
        connect(m_pubName[k], &QLineEdit::editingFinished, this, &ViewportOutputPanel::applyPublish);
    }
    pv->addLayout(grid);
    auto *form = new QFormLayout;
    m_omtQuality = new QComboBox;
    for (const auto &[label, q] : std::initializer_list<std::pair<const char *, int>>{
             {"Auto", 0}, {"Low", 1}, {"Medium", 50}, {"High", 100}})
        m_omtQuality->addItem(QString::fromUtf8(label), q);
    m_omtQuality->setCurrentIndex(qMax(0, m_omtQuality->findData(ps.omtQuality)));
    form->addRow(QStringLiteral("OMT Quality"), m_omtQuality);
    auto *lib = new QHBoxLayout;
    m_libFolder = new QLineEdit(ps.libraryFolder);
    m_libFolder->setPlaceholderText(QStringLiteral("standard locations"));
    m_libFolder->setToolTip(QStringLiteral("Additional folder to search for the NDI (libndi) and OMT (libomt, libvmx) libraries"));
    auto *browse = new QPushButton(QStringLiteral("…"));
    browse->setFixedWidth(30);
    lib->addWidget(m_libFolder, 1);
    lib->addWidget(browse);
    form->addRow(QStringLiteral("Libraries"), lib);
    pv->addLayout(form);
    m_libInfo = note(QString());
    pv->addWidget(m_libInfo);
    pv->addWidget(note(QStringLiteral("NDI and OMT send the picture at the viewport's size; "
                                      "Syphon and Spout share it directly on the GPU.")));
    v->addWidget(pub);
    connect(m_omtQuality, qOverload<int>(&QComboBox::activated), this, &ViewportOutputPanel::applyPublish);
    connect(m_libFolder, &QLineEdit::editingFinished, this, &ViewportOutputPanel::applyPublish);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("NDI / OMT Library Folder"),
                                                            m_libFolder->text());
        if (d.isEmpty()) return;
        m_libFolder->setText(d);
        applyPublish();
    });
    v->addStretch();
    refreshStatus();
}

int ViewportOutputPanel::index() const { return m_engine->indexOfId(m_viewport); }

void ViewportOutputPanel::applySize()
{
    if (m_syncing) return;
    const QSize s(m_width->value(), m_height->value());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(s)));
    {
        Engine::Lock lk(&m_engine->mutex());
        const Layer *l = m_engine->layer(index());
        if (!l || s == l->viewportSize()) return;
    }
    m_engine->setViewportSize(index(), s);
    emit edited();
}

void ViewportOutputPanel::applyPublish()
{
    PublishSettings s = m_engine->publishSettings(m_viewport);
    for (int k = 0; k < kPublishKindCount; ++k) {
        s.targets[k].enabled = m_pubEnabled[k]->isChecked();
        const QString n = m_pubName[k]->text().trimmed();
        s.targets[k].name = n.isEmpty() ? QStringLiteral("Fulskrin") : n;
    }
    s.omtQuality = m_omtQuality->currentData().toInt();
    s.libraryFolder = m_libFolder->text().trimmed();
    if (s == m_engine->publishSettings(m_viewport)) return;
    m_engine->setPublishSettings(m_viewport, s);
    emit edited();
    refreshStatus();
}

void ViewportOutputPanel::refreshStatus()
{
    int mode = 0;
    {
        Engine::Lock lk(&m_engine->mutex());
        const Layer *l = m_engine->layer(index());
        if (!l) return;
        mode = l->vpMode;
    }
    if (QAbstractButton *b = m_mode->button(mode); b && !b->isChecked()) b->setChecked(true);
    for (int k = 0; k < kPublishKindCount; ++k) {
        const PublishState st = m_engine->publishState(m_viewport, PublishKind(k));
        QString text = st.text, color = "#888";
        if (!publishCompiledIn(PublishKind(k))) {
            text = k == int(PublishKind::Syphon) ? QStringLiteral("macOS only") : QStringLiteral("Windows only");
        } else if (st.level == PublishState::Ok) {
            color = "#5fd47a";
            if (st.receivers > 0) text += k == int(PublishKind::Syphon) ? QStringLiteral(" — client connected")
                                                                        : QStringLiteral(" — %1 receiver(s)").arg(st.receivers);
            else if (st.receivers == 0) text += QStringLiteral(" — no receivers");
        } else if (st.level == PublishState::Error) {
            color = "#ff6e5f";
        }
        if (text.isEmpty()) text = QStringLiteral("Disabled");
        m_pubState[k]->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, text.toHtmlEscaped()));
    }
    // Library lookup: roughly every two seconds (disk access)
    if (m_libTick++ % 20 != 0) return;
    const QString folder = m_libFolder->text().trimmed();
    const QString ndi = ndiLibraryPath(folder), omt = omtLibraryPath(folder);
    m_libInfo->setText(QStringLiteral("NDI: %1<br>OMT: %2")
                           .arg(ndi.isEmpty() ? QStringLiteral("not found") : ndi.toHtmlEscaped(),
                                omt.isEmpty() ? QStringLiteral("not found") : omt.toHtmlEscaped()));
}
