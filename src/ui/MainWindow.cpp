#include "MainWindow.h"
#include "Commands.h"
#include "Engine.h"
#include "LayerInspector.h"
#include "MappingView.h"
#include "OutputWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

static const QStringList kVideoExt = {"mov", "mp4", "m4v", "avi", "mkv", "webm", "mxf", "mpg", "mpeg", "wmv", "flv", "ts", "hap"};
static const QStringList kImageExt = {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "webp", "tga"};
static const QStringList kIsfExt = {"fs", "frag"};

static QString videoFilter()
{
    QStringList p;
    for (const QString &e : kVideoExt) p << "*." + e;
    return QStringLiteral("Vidéos (%1);;Tous les fichiers (*)").arg(p.join(' '));
}

static QString imageFilter()
{
    QStringList p;
    for (const QString &e : kImageExt) p << "*." + e;
    return QStringLiteral("Images (%1)").arg(p.join(' '));
}

MainWindow::MainWindow(Engine *engine, QWidget *parent) : QMainWindow(parent), m_engine(engine)
{
    setAcceptDrops(true);
    resize(1500, 900);
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(500);

    QSettings s;
    m_engine->library().setUserFolders(s.value("isf/folders").toStringList());
    m_engine->library().scan();
    m_screenName = s.value("output/screen").toString();

    // --- Panneau des calques
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(8, 8, 4, 8);
    auto *title = new QLabel(QStringLiteral("<b>Calques</b>"));
    m_layers = new QListWidget;
    m_layers->setSelectionMode(QAbstractItemView::SingleSelection);
    m_layers->setToolTip(QStringLiteral("Le calque du haut est affiché au-dessus des autres"));
    auto *bar = new QHBoxLayout;
    auto *add = new QToolButton;
    add->setText(QStringLiteral("+"));
    add->setToolTip(QStringLiteral("Nouveau calque"));
    add->setPopupMode(QToolButton::InstantPopup);
    m_addMenu = new QMenu(add);
    add->setMenu(m_addMenu);
    auto *remove = new QToolButton;
    remove->setText(QStringLiteral("−"));
    remove->setToolTip(QStringLiteral("Supprimer le calque"));
    auto *dup = new QToolButton;
    dup->setText(QStringLiteral("⧉"));
    dup->setToolTip(QStringLiteral("Dupliquer le calque"));
    auto *up = new QToolButton;
    up->setText(QStringLiteral("▲"));
    auto *down = new QToolButton;
    down->setText(QStringLiteral("▼"));
    for (auto *b : {add, remove, dup, up, down}) {
        b->setMinimumSize(30, 26);
        bar->addWidget(b);
    }
    bar->addStretch();
    lv->addWidget(title);
    lv->addWidget(m_layers, 1);
    lv->addLayout(bar);
    lv->addWidget(buildMasterPanel());

    // --- Vue de mapping
    m_view = new MappingView(m_engine);
    m_view->setUndoStack(m_undo);

    // --- Inspecteur
    m_inspector = new LayerInspector(m_engine, m_undo);
    auto *scroll = new QScrollArea;
    scroll->setWidget(m_inspector);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(380);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *split = new QSplitter;
    split->addWidget(left);
    split->addWidget(m_view);
    split->addWidget(scroll);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({240, 850, 410});
    setCentralWidget(split);

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    // --- Sortie : le fil de rendu y présente l'image lui-même
    m_output = new OutputWindow(m_engine);
    connect(m_output, &OutputWindow::closeRequested, this, [this] { setOutputVisible(false); });
    // Quand la fenêtre de sortie a le focus (clic sur l'écran du projecteur), les commandes de régie restent actives.
    connect(m_output, &OutputWindow::keyPressed, this, [this](int key, Qt::KeyboardModifiers mods) {
        const bool ctrl = mods & Qt::ControlModifier;
        if (ctrl && key == Qt::Key_B) setBlackout(!m_blackoutButton->isChecked());
        else if (ctrl && (mods & Qt::ShiftModifier) && key == Qt::Key_F) setOutputVisible(false);
        else if (key == Qt::Key_Space) togglePlayCurrent();
    });

    buildMenus();
    rebuildGeneratorMenus();

    connect(add, &QToolButton::clicked, add, &QToolButton::showMenu);
    connect(remove, &QToolButton::clicked, this, &MainWindow::removeCurrentLayer);
    connect(dup, &QToolButton::clicked, this, &MainWindow::duplicateCurrentLayer);
    connect(up, &QToolButton::clicked, this, [this] { moveCurrentLayer(-1); });
    connect(down, &QToolButton::clicked, this, [this] { moveCurrentLayer(+1); });

    connect(m_layers, &QListWidget::currentRowChanged, this, [this](int r) {
        if (m_refreshingList) return;
        m_view->setLayer(r);
        m_inspector->setLayer(r);
    });
    connect(m_layers, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_refreshingList) return;
        const int row = m_layers->row(it);
        const bool on = it->checkState() == Qt::Checked;
        const QVariant before = cmd::SetLayerProp::read(m_engine, row, cmd::SetLayerProp::Visible);
        if (before.isValid() && before.toBool() != on)
            m_undo->push(new cmd::SetLayerProp(m_engine, row, cmd::SetLayerProp::Visible, before, on));
        if (row == m_inspector->layerIndex()) m_inspector->rebuild();
    });
    connect(m_view, &MappingView::layerPicked, this, &MainWindow::selectLayer);
    connect(m_inspector, &LayerInspector::layerChanged, this, &MainWindow::refreshLayerList);
    connect(m_inspector, &LayerInspector::mappingChanged, m_view, qOverload<>(&QWidget::update));
    connect(m_inspector, &LayerInspector::addSourceRequested, this, &MainWindow::setSourceFromDialog);
    connect(m_engine, &Engine::layersChanged, this, &MainWindow::refreshLayerList);
    connect(m_engine, &Engine::compositionSizeChanged, this, [this] { m_view->update(); });
    // L'aperçu suit le rendu (au plus une mise à jour par image produite)
    connect(m_engine, &Engine::frameRendered, this, [this] {
        m_engine->acknowledgeFrame();
        m_view->update();
    });
    connect(m_undo, &QUndoStack::cleanChanged, this, [this] { updateTitle(); });

    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, &MainWindow::togglePlayCurrent);

    connect(qApp, &QGuiApplication::screenAdded, this, &MainWindow::buildOutputScreensMenu);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        buildOutputScreensMenu();
        if (m_output->isVisible()) setOutputVisible(true); // repositionne
    });

    // Interface : état et transport. Le rendu, lui, ne dépend pas de l'interface.
    m_statusTimer.setInterval(100);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::statusTick);
    m_statusTimer.start();
    if (!m_engine->isThreaded()) {
        // Plateforme sans rendu OpenGL dans un fil séparé : rendu cadencé par l'interface.
        m_renderTimer.setTimerType(Qt::PreciseTimer);
        m_renderTimer.setInterval(16);
        connect(&m_renderTimer, &QTimer::timeout, m_engine, &Engine::renderFrame);
        m_renderTimer.start();
    }
    m_autosaveTimer.setInterval(10000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer.start();

    refreshLayerList();
    updateTitle();
    restoreGeometry(s.value("ui/geometry").toByteArray());
}

MainWindow::~MainWindow()
{
    m_statusTimer.stop();
    m_renderTimer.stop();
    m_autosaveTimer.stop();
    delete m_output;
}

// ---------------------------------------------------------------------------
// Master
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildMasterPanel()
{
    auto *g = new QGroupBox(QStringLiteral("Master"));
    auto *v = new QVBoxLayout(g);
    auto *row = new QHBoxLayout;
    m_masterSlider = new QSlider(Qt::Horizontal);
    m_masterSlider->setRange(0, 100);
    m_masterSlider->setValue(100);
    m_masterSlider->setToolTip(QStringLiteral("Niveau général de la sortie"));
    m_masterLabel = new QLabel(QStringLiteral("100 %"));
    m_masterLabel->setMinimumWidth(44);
    m_masterLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_masterSlider, 1);
    row->addWidget(m_masterLabel);
    v->addLayout(row);

    auto *row2 = new QHBoxLayout;
    m_blackoutButton = new QPushButton(QStringLiteral("Noir"));
    m_blackoutButton->setCheckable(true);
    m_blackoutButton->setToolTip(QStringLiteral("Fondu au noir / retour (Ctrl+B)"));
    m_blackoutButton->setStyleSheet("QPushButton:checked { background:#b3261e; color:white; font-weight:bold; }");
    m_fadeTime = new QDoubleSpinBox;
    m_fadeTime->setRange(0.0, 30.0);
    m_fadeTime->setSingleStep(0.5);
    m_fadeTime->setDecimals(1);
    m_fadeTime->setSuffix(QStringLiteral(" s"));
    m_fadeTime->setValue(QSettings().value("master/fade", 1.0).toDouble());
    m_fadeTime->setToolTip(QStringLiteral("Durée du fondu au noir"));
    row2->addWidget(m_blackoutButton, 1);
    row2->addWidget(new QLabel(QStringLiteral("Fondu")));
    row2->addWidget(m_fadeTime);
    v->addLayout(row2);

    connect(m_masterSlider, &QSlider::valueChanged, this, [this](int val) {
        // Pendant un noir, le curseur règle le niveau de retour sans rallumer.
        if (!m_blackoutButton->isChecked()) m_engine->fadeMaster(val / 100.0, 0.05);
    });
    connect(m_blackoutButton, &QPushButton::toggled, this, &MainWindow::setBlackout);
    connect(m_fadeTime, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [](double v) { QSettings().setValue("master/fade", v); });
    return g;
}

void MainWindow::setBlackout(bool on)
{
    if (m_blackoutButton->isChecked() != on) {
        QSignalBlocker b(m_blackoutButton);
        m_blackoutButton->setChecked(on);
    }
    if (m_blackoutAction && m_blackoutAction->isChecked() != on) {
        QSignalBlocker b(m_blackoutAction);
        m_blackoutAction->setChecked(on);
    }
    m_engine->fadeMaster(on ? 0.0 : m_masterSlider->value() / 100.0, m_fadeTime->value());
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(QStringLiteral("&Fichier"));
    file->addAction(QStringLiteral("Nouveau projet"), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(QStringLiteral("Ouvrir…"), QKeySequence::Open, this, &MainWindow::openProjectDialog);
    file->addSeparator();
    file->addAction(QStringLiteral("Enregistrer"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(QStringLiteral("Enregistrer sous…"), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("Quitter"), QKeySequence::Quit, this, &QWidget::close);
    quit->setMenuRole(QAction::QuitRole);

    // Annuler / rétablir : actions maison pour rafraîchir toute l'interface après coup
    QMenu *edit = menuBar()->addMenu(QStringLiteral("É&dition"));
    m_undoAction = edit->addAction(QStringLiteral("Annuler"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = edit->addAction(QStringLiteral("Rétablir"));
    {
        // Ctrl+Maj+Z et Ctrl+Y partout (Cmd sur Mac), sans doublon avec le raccourci natif
        QList<QKeySequence> redo = QKeySequence::keyBindings(QKeySequence::Redo);
        for (const QKeySequence &k : {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)})
            if (!redo.contains(k)) redo << k;
        m_redoAction->setShortcuts(redo);
    }
    auto updateUndoActions = [this] {
        m_undoAction->setEnabled(m_undo->canUndo());
        m_redoAction->setEnabled(m_undo->canRedo());
        m_undoAction->setText(m_undo->canUndo() ? QStringLiteral("Annuler : %1").arg(m_undo->undoText())
                                                : QStringLiteral("Annuler"));
        m_redoAction->setText(m_undo->canRedo() ? QStringLiteral("Rétablir : %1").arg(m_undo->redoText())
                                                : QStringLiteral("Rétablir"));
    };
    connect(m_undo, &QUndoStack::indexChanged, this, updateUndoActions);
    connect(m_undo, &QUndoStack::undoTextChanged, this, updateUndoActions);
    updateUndoActions();
    connect(m_undoAction, &QAction::triggered, this, [this] {
        m_undo->undo();
        refreshAll();
    });
    connect(m_redoAction, &QAction::triggered, this, [this] {
        m_undo->redo();
        refreshAll();
    });

    QMenu *layer = menuBar()->addMenu(QStringLiteral("&Calque"));
    // Même contenu dans le menu « + » du panneau
    for (QMenu *m : {layer, m_addMenu}) {
        m->addAction(QStringLiteral("Calque vidéo…"), this, &MainWindow::addVideoLayer);
        m->addAction(QStringLiteral("Calque image…"), this, &MainWindow::addImageLayer);
        QMenu *gen = m->addMenu(QStringLiteral("Calque générateur ISF"));
        gen->setProperty("generatorMenu", true);
        m->addAction(QStringLiteral("Calque vide"), this, &MainWindow::addEmptyLayer);
        if (m == layer) m_generatorMenu = gen;
    }
    layer->addSeparator();
    QAction *dup = layer->addAction(QStringLiteral("Dupliquer"), this, &MainWindow::duplicateCurrentLayer);
    dup->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    QAction *del = layer->addAction(QStringLiteral("Supprimer"), this, &MainWindow::removeCurrentLayer);
    del->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    del->setShortcutContext(Qt::WidgetShortcut);
    m_layers->addAction(del);
    layer->addAction(QStringLiteral("Monter"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                     [this] { moveCurrentLayer(-1); });
    layer->addAction(QStringLiteral("Descendre"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                     [this] { moveCurrentLayer(+1); });
    layer->addSeparator();
    layer->addAction(QStringLiteral("Lecture / pause (Espace)"), this, &MainWindow::togglePlayCurrent);

    QMenu *comp = menuBar()->addMenu(QStringLiteral("C&omposition"));
    comp->addAction(QStringLiteral("Résolution…"), this, &MainWindow::compositionDialog);
    QAction *outlines = comp->addAction(QStringLiteral("Afficher le contour des autres calques"));
    outlines->setCheckable(true);
    outlines->setChecked(true);
    connect(outlines, &QAction::toggled, m_view, &MappingView::setShowAllOutlines);

    QMenu *out = menuBar()->addMenu(QStringLiteral("&Sortie"));
    m_outputAction = out->addAction(QStringLiteral("Afficher la sortie"));
    m_outputAction->setCheckable(true);
    m_outputAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(m_outputAction, &QAction::toggled, this, &MainWindow::setOutputVisible);
    m_blackoutAction = out->addAction(QStringLiteral("Noir (fondu)"));
    m_blackoutAction->setCheckable(true);
    m_blackoutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    m_blackoutAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_blackoutAction, &QAction::toggled, this, &MainWindow::setBlackout);
    out->addSeparator();
    m_screensMenu = out->addMenu(QStringLiteral("Écran de sortie"));
    out->addAction(QStringLiteral("Composition = résolution de l'écran de sortie"), this, [this] {
        if (QScreen *sc = selectedScreen()) {
            const QSize px = sc->geometry().size() * sc->devicePixelRatio();
            m_engine->setCompositionSize(px);
            statusBar()->showMessage(QStringLiteral("Composition : %1 × %2").arg(px.width()).arg(px.height()), 4000);
        }
    });
    buildOutputScreensMenu();

    QMenu *lib = menuBar()->addMenu(QStringLiteral("&Bibliothèque ISF"));
    lib->addAction(QStringLiteral("Ajouter un dossier de shaders…"), this, &MainWindow::addIsfFolder);
    lib->addAction(QStringLiteral("Rafraîchir"), QKeySequence::Refresh, this, &MainWindow::rescanLibrary);
    lib->addAction(QStringLiteral("Oublier les dossiers ajoutés"), this, [this] {
        m_engine->library().setUserFolders({});
        QSettings().setValue("isf/folders", QStringList());
        rescanLibrary();
    });
    lib->addSeparator();
    lib->addAction(QStringLiteral("Ouvrir le dossier des shaders fournis"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(IsfLibrary::bundledFolder()));
    });
    connect(lib, &QMenu::aboutToShow, this, [this, lib] {
        // Liste informative des dossiers scannés
        for (QAction *a : lib->actions())
            if (a->property("folderInfo").toBool()) lib->removeAction(a), a->deleteLater();
        for (const QString &f : m_engine->library().allFolders()) {
            QAction *a = lib->addAction(f);
            a->setEnabled(false);
            a->setProperty("folderInfo", true);
        }
    });
}

void MainWindow::rebuildGeneratorMenus()
{
    const QList<QMenu *> menus = findChildren<QMenu *>();
    for (QMenu *m : menus) {
        if (!m->property("generatorMenu").toBool()) continue;
        m->clear();
        for (const IsfEntry &e : m_engine->library().generators()) {
            QAction *a = m->addAction(e.name);
            a->setToolTip(e.description);
            connect(a, &QAction::triggered, this, [this, p = e.path] { addGeneratorLayer(p); });
        }
        if (m->isEmpty()) m->addAction(QStringLiteral("(aucun générateur)"))->setEnabled(false);
    }
}

void MainWindow::buildOutputScreensMenu()
{
    m_screensMenu->clear();
    delete m_screenGroup;
    m_screenGroup = new QActionGroup(this);
    const auto screens = QGuiApplication::screens();
    bool found = false;
    for (QScreen *sc : screens) found |= sc->name() == m_screenName;
    if (!found) {
        // Par défaut : le premier écran qui n'est pas l'écran principal (vidéoprojecteur)
        m_screenName = screens.size() > 1 ? screens.at(1)->name() : QGuiApplication::primaryScreen()->name();
    }
    for (QScreen *sc : screens) {
        const QSize px = sc->geometry().size() * sc->devicePixelRatio();
        QAction *a = m_screensMenu->addAction(
            QStringLiteral("%1  (%2 × %3)").arg(sc->name()).arg(px.width()).arg(px.height()));
        a->setCheckable(true);
        a->setChecked(sc->name() == m_screenName);
        m_screenGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, name = sc->name()] {
            m_screenName = name;
            QSettings().setValue("output/screen", name);
            if (m_output->isVisible()) setOutputVisible(true);
        });
    }
}

QScreen *MainWindow::selectedScreen() const
{
    for (QScreen *sc : QGuiApplication::screens())
        if (sc->name() == m_screenName) return sc;
    return QGuiApplication::primaryScreen();
}

void MainWindow::setOutputVisible(bool on)
{
    if (m_outputAction->isChecked() != on) {
        QSignalBlocker b(m_outputAction);
        m_outputAction->setChecked(on);
    }
    m_autosaveDone = false; // l'état de la sortie fait partie de la session à restaurer
    if (!on) {
        m_output->hideOutput();
        return;
    }
    QScreen *sc = selectedScreen();
    // Sur l'écran de l'interface on reste fenêtré, pour ne pas masquer les commandes.
    const bool sameAsUi = sc == screen();
    m_output->showOn(sc, !sameAsUi);
    if (sameAsUi)
        statusBar()->showMessage(
            QStringLiteral("Sortie fenêtrée : choisissez un autre écran dans Sortie ▸ Écran de sortie pour le plein écran."),
            6000);
    activateWindow();
}

// ---------------------------------------------------------------------------
// Liste des calques
// ---------------------------------------------------------------------------

static QString layerTag(SourceType t)
{
    switch (t) {
    case SourceType::Video: return QStringLiteral("▶");
    case SourceType::Image: return QStringLiteral("▣");
    case SourceType::Isf: return QStringLiteral("◆");
    default: return QStringLiteral("○");
    }
}

void MainWindow::refreshLayerList()
{
    struct Row {
        QString text;
        bool visible, error;
    };
    std::vector<Row> rows;
    {
        Engine::Lock lk(&m_engine->mutex());
        for (int i = 0; i < m_engine->layerCount(); ++i) {
            Layer *l = m_engine->layer(i);
            rows.push_back({QStringLiteral("%1  %2").arg(layerTag(l->type), l->name), l->visible, !l->error.isEmpty()});
        }
    }
    const int n = int(rows.size());
    const int keep = qBound(-1, m_layers->currentRow(), n - 1);
    m_refreshingList = true;
    m_layers->clear();
    for (const Row &r : rows) {
        auto *it = new QListWidgetItem(r.text);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(r.visible ? Qt::Checked : Qt::Unchecked);
        if (r.error) it->setForeground(QColor(255, 120, 100));
        m_layers->addItem(it);
    }
    const int sel = keep >= 0 ? keep : (n > 0 ? 0 : -1);
    m_layers->setCurrentRow(sel);
    m_refreshingList = false;
    if (sel != m_inspector->layerIndex()) {
        m_inspector->setLayer(sel);
        m_view->setLayer(sel);
    }
}

void MainWindow::refreshAll()
{
    refreshLayerList();
    m_inspector->rebuild();
    m_view->update();
}

void MainWindow::selectLayer(int index)
{
    refreshLayerList();
    m_refreshingList = true;
    m_layers->setCurrentRow(index);
    m_refreshingList = false;
    m_view->setLayer(index);
    m_inspector->setLayer(index);
}

int MainWindow::currentLayer() const { return m_layers->currentRow(); }

// ---------------------------------------------------------------------------
// Actions sur les calques (toutes annulables)
// ---------------------------------------------------------------------------

int MainWindow::newLayerFromFile(const QString &path, int at)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    const QString base = QFileInfo(path).completeBaseName();
    QString err;
    int idx = m_engine->addLayer(base, at);
    bool ok;
    if (kImageExt.contains(ext)) ok = m_engine->setLayerImage(idx, path, &err);
    else if (kIsfExt.contains(ext)) ok = (m_engine->setLayerIsf(idx, path, &err), true);
    else ok = m_engine->setLayerVideo(idx, path, &err); // extensions vidéo et inconnues : FFmpeg tranche
    if (!ok) {
        m_engine->removeLayer(idx);
        idx = -1;
    } else {
        m_undo->push(new cmd::AddLayer(m_engine, idx, QStringLiteral("Ajouter « %1 »").arg(base)));
    }
    if (!err.isEmpty()) statusBar()->showMessage(QFileInfo(path).fileName() + " : " + err.section('\n', 0, 0), 8000);
    return idx;
}

void MainWindow::addVideoLayer()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Calque vidéo"),
                                                            s.value("dirs/video").toString(), videoFilter());
    if (files.isEmpty()) return;
    s.setValue("dirs/video", QFileInfo(files.first()).absolutePath());
    int last = -1;
    for (int k = files.size() - 1; k >= 0; --k) {
        int i = newLayerFromFile(files[k], 0);
        if (i >= 0) last = i;
    }
    if (last >= 0) selectLayer(last);
}

void MainWindow::addImageLayer()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Calque image"),
                                                            s.value("dirs/image").toString(), imageFilter());
    if (files.isEmpty()) return;
    s.setValue("dirs/image", QFileInfo(files.first()).absolutePath());
    int last = -1;
    for (int k = files.size() - 1; k >= 0; --k) {
        int i = newLayerFromFile(files[k], 0);
        if (i >= 0) last = i;
    }
    if (last >= 0) selectLayer(last);
}

void MainWindow::setSourceFromDialog(const QString &kind)
{
    const int i = currentLayer();
    if (i < 0) return;
    QSettings s;
    const bool video = kind == "video";
    const QString key = video ? "dirs/video" : "dirs/image";
    const QString f = QFileDialog::getOpenFileName(this, video ? QStringLiteral("Source vidéo") : QStringLiteral("Source image"),
                                                   s.value(key).toString(), video ? videoFilter() : imageFilter());
    if (f.isEmpty()) return;
    s.setValue(key, QFileInfo(f).absolutePath());
    const QJsonObject before = m_engine->layerJson(i);
    QString err;
    const bool ok = video ? m_engine->setLayerVideo(i, f, &err) : m_engine->setLayerImage(i, f, &err);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Source"), err);
        return;
    }
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(i);
        if (l && l->name.startsWith(QStringLiteral("Calque "))) l->name = QFileInfo(f).completeBaseName();
    }
    m_undo->push(new cmd::ReplaceLayer(m_engine, i, before, QStringLiteral("Source « %1 »").arg(QFileInfo(f).fileName())));
    refreshAll();
}

void MainWindow::addGeneratorLayer(const QString &path)
{
    QString err;
    const QString name = QFileInfo(path).completeBaseName();
    int i = m_engine->addLayer(name, 0);
    m_engine->setLayerIsf(i, path, &err);
    m_undo->push(new cmd::AddLayer(m_engine, i, QStringLiteral("Ajouter « %1 »").arg(name)));
    selectLayer(i);
}

void MainWindow::addEmptyLayer()
{
    int i = m_engine->addLayer();
    m_undo->push(new cmd::AddLayer(m_engine, i, QStringLiteral("Ajouter un calque")));
    selectLayer(i);
}

void MainWindow::removeCurrentLayer()
{
    const int i = currentLayer();
    if (i < 0) return;
    m_undo->push(new cmd::RemoveLayer(m_engine, i));
    selectLayer(qMin(i, m_engine->layerCount() - 1));
}

void MainWindow::duplicateCurrentLayer()
{
    const int i = currentLayer();
    if (i < 0) return;
    const int ni = m_engine->duplicateLayer(i);
    if (ni < 0) return;
    m_undo->push(new cmd::AddLayer(m_engine, ni, QStringLiteral("Dupliquer le calque")));
    selectLayer(ni);
}

void MainWindow::moveCurrentLayer(int delta)
{
    const int i = currentLayer();
    const int to = i + delta;
    if (i < 0 || to < 0 || to >= m_engine->layerCount()) return;
    m_undo->push(new cmd::MoveLayer(m_engine, i, to));
    selectLayer(to);
}

void MainWindow::togglePlayCurrent()
{
    const int i = currentLayer();
    bool playing;
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(i);
        if (!l || !l->video) return;
        playing = l->playing;
    }
    m_engine->setLayerPlaying(i, !playing);
    m_inspector->refreshDynamic();
}

// ---------------------------------------------------------------------------
// État
// ---------------------------------------------------------------------------

void MainWindow::statusTick()
{
    m_inspector->refreshDynamic();
    const double lvl = m_engine->masterLevel();
    const int pct = int(std::lround(lvl * 100));
    m_masterLabel->setText(QStringLiteral("%1 %").arg(pct));
    m_masterLabel->setStyleSheet(pct == 0 ? "color:#ff5a4f; font-weight:bold;" : "");
    const QSize c = m_engine->compositionSize();
    m_status->setText(QStringLiteral("%1 × %2   ·   %3 i/s   ·   master %4 %   ·   sortie : %5")
                          .arg(c.width())
                          .arg(c.height())
                          .arg(m_engine->fps(), 0, 'f', 1)
                          .arg(pct)
                          .arg(m_output->isVisible() ? m_screenName : QStringLiteral("masquée")));
}

// ---------------------------------------------------------------------------
// Composition, bibliothèque
// ---------------------------------------------------------------------------

void MainWindow::compositionDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Résolution de la composition"));
    auto *form = new QFormLayout(&dlg);
    auto *preset = new QComboBox;
    const QList<QSize> sizes = {{1920, 1080}, {1280, 720}, {3840, 2160}, {1920, 1200}, {2560, 1600},
                                {1400, 1050}, {1024, 768}, {4096, 2160}, {3840, 1080}, {5760, 1080}};
    preset->addItem(QStringLiteral("Personnalisée"));
    for (const QSize &s : sizes) preset->addItem(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()), s);
    auto *w = new QSpinBox, *h = new QSpinBox;
    w->setRange(16, 16384);
    h->setRange(16, 16384);
    w->setValue(m_engine->compositionSize().width());
    h->setValue(m_engine->compositionSize().height());
    form->addRow(QStringLiteral("Préréglage"), preset);
    form->addRow(QStringLiteral("Largeur"), w);
    form->addRow(QStringLiteral("Hauteur"), h);
    auto *note = new QLabel(QStringLiteral("Réglez-la sur la résolution native du vidéoprojecteur.\n"
                                           "Le mapping est stocké en coordonnées relatives : il suit le changement."));
    note->setStyleSheet("color:#999;");
    form->addRow(note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(preset, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [=](int i) {
        const QSize s = preset->itemData(i).toSize();
        if (s.isValid()) {
            w->setValue(s.width());
            h->setValue(s.height());
        }
    });
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        m_engine->setCompositionSize(QSize(w->value(), h->value()));
        m_forceDirty = true;
        m_autosaveDone = false;
        updateTitle();
    }
}

void MainWindow::addIsfFolder()
{
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Dossier de shaders ISF"));
    if (d.isEmpty()) return;
    QStringList f = m_engine->library().userFolders();
    if (!f.contains(d)) f << d;
    m_engine->library().setUserFolders(f);
    QSettings().setValue("isf/folders", f);
    rescanLibrary();
}

void MainWindow::rescanLibrary()
{
    m_engine->library().scan();
    rebuildGeneratorMenus();
    m_inspector->rebuild();
    statusBar()->showMessage(QStringLiteral("Bibliothèque ISF : %1 générateurs, %2 effets")
                                 .arg(m_engine->library().generators().size())
                                 .arg(m_engine->library().filters().size()),
                             5000);
}

// ---------------------------------------------------------------------------
// Projet, sauvegarde automatique, reprise
// ---------------------------------------------------------------------------

bool MainWindow::isDirty() const { return m_forceDirty || !m_undo->isClean(); }

void MainWindow::updateTitle()
{
    const QString p = m_engine->projectPath();
    setWindowTitle((p.isEmpty() ? QStringLiteral("sans titre") : QFileInfo(p).fileName()) + QStringLiteral("[*] — Lanterne"));
    setWindowModified(isDirty());
}

QJsonObject MainWindow::uiState() const
{
    QJsonObject o;
    o["outputScreen"] = m_screenName;
    o["selectedLayer"] = currentLayer();
    o["outputVisible"] = m_output && m_output->isVisible();
    return o;
}

bool MainWindow::maybeSave()
{
    if (!isDirty()) return true;
    auto r = QMessageBox::question(this, QStringLiteral("Lanterne"), QStringLiteral("Enregistrer les modifications du projet ?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_engine->newProject();
    m_undo->clear();
    m_forceDirty = false;
    m_autosaveDone = false;
    selectLayer(-1);
    updateTitle();
}

void MainWindow::openProjectDialog()
{
    if (!maybeSave()) return;
    QSettings s;
    const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Ouvrir un projet"), s.value("dirs/project").toString(),
                                                   QStringLiteral("Projets Lanterne (*.lanterne *.json)"));
    if (!f.isEmpty()) openProject(f);
}

bool MainWindow::openProject(const QString &path)
{
    QJsonObject ui;
    QString err;
    if (!m_engine->loadProject(path, &ui, &err) && !err.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Ouverture"), err);
        return false;
    }
    if (!err.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Ouverture"), QStringLiteral("Projet ouvert avec des avertissements :\n\n") + err);
    m_undo->clear();
    m_forceDirty = false;
    m_autosaveDone = false;
    QSettings s;
    s.setValue("dirs/project", QFileInfo(path).absolutePath());
    s.setValue("project/last", QFileInfo(path).absoluteFilePath());
    const QString scr = ui.value("outputScreen").toString();
    if (!scr.isEmpty()) {
        m_screenName = scr;
        buildOutputScreensMenu();
    }
    selectLayer(qBound(-1, ui.value("selectedLayer").toInt(0), m_engine->layerCount() - 1));
    updateTitle();
    return true;
}

bool MainWindow::saveTo(const QString &path)
{
    QString err;
    if (!m_engine->saveProject(path, uiState(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Enregistrement"), err);
        return false;
    }
    m_engine->setProjectPath(QFileInfo(path).absoluteFilePath());
    m_undo->setClean();
    m_forceDirty = false;
    updateTitle();
    statusBar()->showMessage(QStringLiteral("Enregistré : ") + path, 4000);
    return true;
}

bool MainWindow::save()
{
    const QString p = m_engine->projectPath();
    return p.isEmpty() ? saveAs() : saveTo(p);
}

bool MainWindow::saveAs()
{
    QSettings s;
    QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Enregistrer le projet"), s.value("dirs/project").toString(),
                                             QStringLiteral("Projets Lanterne (*.lanterne)"));
    if (f.isEmpty()) return false;
    if (QFileInfo(f).suffix().isEmpty()) f += ".lanterne";
    if (!saveTo(f)) return false;
    s.setValue("dirs/project", QFileInfo(f).absolutePath());
    s.setValue("project/last", f);
    return true;
}

QString MainWindow::autosavePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/autosave.lanterne";
}

void MainWindow::autosave()
{
    if (!m_autosaveEnabled) return;
    if (m_autosaveDone && m_undo->index() == m_autosaveIndex) return; // rien de neuf
    QJsonObject ui = uiState();
    ui["autosaveOf"] = m_engine->projectPath();
    ui["autosaveTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    ui["autosaveDirty"] = isDirty();
    QString err;
    if (m_engine->saveProject(autosavePath(), ui, &err)) {
        m_autosaveIndex = m_undo->index();
        m_autosaveDone = true;
    } else {
        statusBar()->showMessage(QStringLiteral("Sauvegarde automatique impossible : ") + err, 6000);
    }
}

void MainWindow::offerRecovery()
{
    const QString path = autosavePath();
    if (!QFile::exists(path)) return;
    QFile f(path);
    QJsonObject ui;
    if (f.open(QIODevice::ReadOnly)) ui = QJsonDocument::fromJson(f.readAll()).object().value("ui").toObject();
    f.close();
    const QString original = ui.value("autosaveOf").toString();
    const QDateTime when = QDateTime::fromString(ui.value("autosaveTime").toString(), Qt::ISODate);
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Reprise"),
                    QStringLiteral("Lanterne ne s'est pas fermée normalement."),
                    QMessageBox::NoButton, this);
    box.setInformativeText(QStringLiteral("Restaurer la session sauvegardée automatiquement le %1 %2 ?")
                               .arg(when.isValid() ? when.toString("dd/MM à HH:mm:ss") : QStringLiteral("(date inconnue)"),
                                    original.isEmpty() ? QString() : QStringLiteral("(projet %1)").arg(QFileInfo(original).fileName())));
    QPushButton *restore = box.addButton(QStringLiteral("Restaurer"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Ignorer"), QMessageBox::RejectRole);
    box.setDefaultButton(restore);
    box.exec();
    if (box.clickedButton() != restore) {
        QFile::remove(path);
        return;
    }
    QJsonObject loadedUi;
    QString err;
    m_engine->loadProject(path, &loadedUi, &err);
    m_engine->setProjectPath(original); // « Enregistrer » écrit dans le projet d'origine
    m_undo->clear();
    m_forceDirty = ui.value("autosaveDirty").toBool(true);
    m_autosaveDone = false;
    const QString scr = loadedUi.value("outputScreen").toString();
    if (!scr.isEmpty()) {
        m_screenName = scr;
        buildOutputScreensMenu();
    }
    selectLayer(qBound(-1, loadedUi.value("selectedLayer").toInt(0), m_engine->layerCount() - 1));
    updateTitle();
    if (!err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Reprise"), err);
    if (ui.value("outputVisible").toBool()) {
        // La sortie revient sur le projecteur, mais au noir : c'est la régie qui décide de rallumer.
        setBlackout(true);
        m_engine->fadeMaster(0.0, 0.0);
        setOutputVisible(true);
        statusBar()->showMessage(QStringLiteral("Session restaurée — sortie au noir : Ctrl+B pour rallumer"), 15000);
    } else {
        statusBar()->showMessage(QStringLiteral("Session restaurée"), 6000);
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings().setValue("ui/geometry", saveGeometry());
    m_output->hideOutput();
    // Fermeture normale : pas de reprise à proposer au prochain lancement.
    if (m_autosaveEnabled) QFile::remove(autosavePath());
    e->accept();
}

// ---------------------------------------------------------------------------
// Glisser-déposer
// ---------------------------------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    int last = -1;
    const QList<QUrl> urls = e->mimeData()->urls();
    for (int k = urls.size() - 1; k >= 0; --k) {
        const QString path = urls[k].toLocalFile();
        if (path.isEmpty()) continue;
        const QString ext = QFileInfo(path).suffix().toLower();
        if (ext == "lanterne") {
            if (maybeSave()) openProject(path);
            return;
        }
        // Un filtre ISF déposé s'ajoute comme effet au calque sélectionné
        const int cur = currentLayer();
        if (kIsfExt.contains(ext) && IsfInstance::readHeader(path).isFilter && cur >= 0) {
            const QJsonArray before = m_engine->effectsJson(cur);
            m_engine->addEffect(cur, path);
            m_undo->push(new cmd::SetEffects(m_engine, cur, before,
                                             QStringLiteral("Ajouter l'effet %1").arg(QFileInfo(path).completeBaseName())));
            m_inspector->rebuild();
            continue;
        }
        int i = newLayerFromFile(path, 0);
        if (i >= 0) last = i;
    }
    if (last >= 0) selectLayer(last);
    e->acceptProposedAction();
}
