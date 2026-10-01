#include "MainWindow.h"
#include "Engine.h"
#include "LayerInspector.h"
#include "MappingView.h"
#include "OutputWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QComboBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
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

    // --- Vue de mapping
    m_view = new MappingView(m_engine);

    // --- Inspecteur
    m_inspector = new LayerInspector(m_engine);
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
    split->setSizes({230, 860, 410});
    setCentralWidget(split);

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    // --- Sortie
    m_output = new OutputWindow(m_engine);
    connect(m_output, &OutputWindow::frameSwapped, this, [this] {
        if (m_output->isVisible()) tick();
    });
    connect(m_output, &OutputWindow::closeRequested, this, [this] { setOutputVisible(false); });

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
        if (Layer *l = m_engine->layer(m_layers->row(it))) {
            l->visible = it->checkState() == Qt::Checked;
            if (m_layers->row(it) == m_inspector->layerIndex()) m_inspector->rebuild();
        }
    });
    connect(m_view, &MappingView::layerPicked, this, &MainWindow::selectLayer);
    connect(m_inspector, &LayerInspector::layerChanged, this, &MainWindow::refreshLayerList);
    connect(m_inspector, &LayerInspector::mappingChanged, m_view, qOverload<>(&QWidget::update));
    connect(m_inspector, &LayerInspector::addSourceRequested, this, &MainWindow::setSourceFromDialog);
    connect(m_engine, &Engine::layersChanged, this, &MainWindow::refreshLayerList);
    connect(m_engine, &Engine::compositionSizeChanged, this, [this] { m_view->update(); });

    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, &MainWindow::togglePlayCurrent);

    connect(qApp, &QGuiApplication::screenAdded, this, &MainWindow::buildOutputScreensMenu);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        buildOutputScreensMenu();
        if (m_output->isVisible()) setOutputVisible(true); // repositionne
    });

    // Horloge quand la sortie est masquée ; sinon c'est la synchro verticale de la sortie qui cadence.
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &MainWindow::tick);
    m_timer.start();

    refreshLayerList();
    updateTitle();
    restoreGeometry(s.value("ui/geometry").toByteArray());
}

MainWindow::~MainWindow()
{
    m_timer.stop();
    delete m_output;
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

    QMenu *layer = menuBar()->addMenu(QStringLiteral("&Calque"));
    // Même contenu dans le menu « + » du panneau
    for (QMenu *m : {layer, m_addMenu}) {
        m->addAction(QStringLiteral("Calque vidéo…"), this, &MainWindow::addVideoLayer);
        m->addAction(QStringLiteral("Calque image…"), this, &MainWindow::addImageLayer);
        QMenu *gen = m->addMenu(QStringLiteral("Calque générateur ISF"));
        gen->setProperty("generatorMenu", true);
        m->addAction(QStringLiteral("Calque vide"), this, [this] { selectLayer(m_engine->addLayer()); });
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
    if (!on) {
        m_output->hide();
        m_timer.start();
        return;
    }
    QScreen *sc = selectedScreen();
    // Sur l'écran de l'interface on reste fenêtré, pour ne pas masquer les commandes.
    const bool sameAsUi = sc == screen();
    m_output->showOn(sc, !sameAsUi);
    m_timer.stop();
    m_output->update();
    if (sameAsUi)
        statusBar()->showMessage(
            QStringLiteral("Sortie fenêtrée : choisissez un autre écran dans Sortie ▸ Écran de sortie pour le plein écran."),
            6000);
    activateWindow();
}

// ---------------------------------------------------------------------------
// Liste des calques
// ---------------------------------------------------------------------------

static QString layerTag(const Layer *l)
{
    switch (l->type) {
    case SourceType::Video: return QStringLiteral("▶");
    case SourceType::Image: return QStringLiteral("▣");
    case SourceType::Isf: return QStringLiteral("◆");
    default: return QStringLiteral("○");
    }
}

void MainWindow::refreshLayerList()
{
    const int keep = qBound(-1, m_layers->currentRow(), m_engine->layerCount() - 1);
    m_refreshingList = true;
    m_layers->clear();
    for (int i = 0; i < m_engine->layerCount(); ++i) {
        Layer *l = m_engine->layer(i);
        auto *it = new QListWidgetItem(QStringLiteral("%1  %2").arg(layerTag(l), l->name));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(l->visible ? Qt::Checked : Qt::Unchecked);
        if (!l->error.isEmpty()) it->setForeground(QColor(255, 120, 100));
        m_layers->addItem(it);
    }
    int sel = keep >= 0 ? keep : (m_engine->layerCount() > 0 ? 0 : -1);
    m_layers->setCurrentRow(sel);
    m_refreshingList = false;
    if (sel != m_inspector->layerIndex()) {
        m_inspector->setLayer(sel);
        m_view->setLayer(sel);
    }
}

void MainWindow::selectLayer(int index)
{
    m_refreshingList = true;
    refreshLayerList();
    m_refreshingList = true;
    m_layers->setCurrentRow(index);
    m_refreshingList = false;
    m_view->setLayer(index);
    m_inspector->setLayer(index);
}

int MainWindow::currentLayer() const { return m_layers->currentRow(); }

// ---------------------------------------------------------------------------
// Actions sur les calques
// ---------------------------------------------------------------------------

int MainWindow::newLayerFromFile(const QString &path, int at)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    const QString base = QFileInfo(path).completeBaseName();
    QString err;
    int idx = -1;
    if (kVideoExt.contains(ext)) {
        idx = m_engine->addLayer(base, at);
        if (!m_engine->setLayerVideo(idx, path, &err)) {
            // Extension inconnue mais lisible ? On tente l'image, sinon on signale.
            m_engine->removeLayer(idx);
            idx = -1;
        }
    } else if (kImageExt.contains(ext)) {
        idx = m_engine->addLayer(base, at);
        if (!m_engine->setLayerImage(idx, path, &err)) {
            m_engine->removeLayer(idx);
            idx = -1;
        }
    } else if (kIsfExt.contains(ext)) {
        idx = m_engine->addLayer(base, at);
        m_engine->setLayerIsf(idx, path, &err);
    } else {
        idx = m_engine->addLayer(base, at);
        if (!m_engine->setLayerVideo(idx, path, &err)) {
            m_engine->removeLayer(idx);
            idx = -1;
        }
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
    QString err;
    const bool ok = video ? m_engine->setLayerVideo(i, f, &err) : m_engine->setLayerImage(i, f, &err);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Source"), err);
        return;
    }
    Layer *l = m_engine->layer(i);
    if (l->name.startsWith(QStringLiteral("Calque "))) l->name = QFileInfo(f).completeBaseName();
    refreshLayerList();
    m_inspector->rebuild();
}

void MainWindow::addGeneratorLayer(const QString &path)
{
    QString err;
    int i = m_engine->addLayer(QFileInfo(path).completeBaseName(), 0);
    m_engine->setLayerIsf(i, path, &err);
    selectLayer(i);
}

void MainWindow::removeCurrentLayer()
{
    const int i = currentLayer();
    if (i < 0) return;
    m_engine->removeLayer(i);
    selectLayer(qMin(i, m_engine->layerCount() - 1));
}

void MainWindow::duplicateCurrentLayer()
{
    const int i = currentLayer();
    if (i < 0) return;
    selectLayer(m_engine->duplicateLayer(i));
}

void MainWindow::moveCurrentLayer(int delta)
{
    const int i = currentLayer();
    const int to = i + delta;
    if (i < 0 || to < 0 || to >= m_engine->layerCount()) return;
    m_engine->moveLayer(i, to);
    selectLayer(to);
}

void MainWindow::togglePlayCurrent()
{
    const int i = currentLayer();
    Layer *l = m_engine->layer(i);
    if (!l || !l->video) return;
    m_engine->setLayerPlaying(i, !l->playing);
    m_inspector->refreshDynamic();
}

// ---------------------------------------------------------------------------
// Horloge
// ---------------------------------------------------------------------------

void MainWindow::tick()
{
    m_engine->renderFrame();
    m_view->update();
    if (m_output->isVisible()) m_output->update();
    if (++m_tickCount % 6 == 0) {
        m_inspector->refreshDynamic();
        const QSize c = m_engine->compositionSize();
        m_status->setText(QStringLiteral("%1 × %2   ·   %3 i/s   ·   sortie : %4")
                              .arg(c.width())
                              .arg(c.height())
                              .arg(m_engine->fps(), 0, 'f', 1)
                              .arg(m_output->isVisible() ? m_screenName : QStringLiteral("masquée")));
    }
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
    if (dlg.exec() == QDialog::Accepted) m_engine->setCompositionSize(QSize(w->value(), h->value()));
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
// Projet
// ---------------------------------------------------------------------------

void MainWindow::updateTitle()
{
    const QString p = m_engine->projectPath();
    setWindowTitle(p.isEmpty() ? QStringLiteral("Lanterne — sans titre") : QStringLiteral("Lanterne — ") + QFileInfo(p).fileName());
}

QJsonObject MainWindow::uiState() const
{
    QJsonObject o;
    o["outputScreen"] = m_screenName;
    o["selectedLayer"] = currentLayer();
    return o;
}

bool MainWindow::maybeSave()
{
    if (m_engine->layerCount() == 0) return true;
    auto r = QMessageBox::question(this, QStringLiteral("Lanterne"), QStringLiteral("Enregistrer le projet en cours ?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_engine->newProject();
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

bool MainWindow::save()
{
    if (m_engine->projectPath().isEmpty()) return saveAs();
    QString err;
    if (!m_engine->saveProject(m_engine->projectPath(), uiState(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Enregistrement"), err);
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Enregistré : ") + m_engine->projectPath(), 4000);
    return true;
}

bool MainWindow::saveAs()
{
    QSettings s;
    QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Enregistrer le projet"), s.value("dirs/project").toString(),
                                             QStringLiteral("Projets Lanterne (*.lanterne)"));
    if (f.isEmpty()) return false;
    if (QFileInfo(f).suffix().isEmpty()) f += ".lanterne";
    QString err;
    if (!m_engine->saveProject(f, uiState(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Enregistrement"), err);
        return false;
    }
    s.setValue("dirs/project", QFileInfo(f).absolutePath());
    s.setValue("project/last", f);
    updateTitle();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings().setValue("ui/geometry", saveGeometry());
    m_output->hide();
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
        if (kIsfExt.contains(ext) && IsfInstance::readHeader(path).isFilter && currentLayer() >= 0) {
            m_engine->addEffect(currentLayer(), path);
            m_inspector->rebuild();
            continue;
        }
        int i = newLayerFromFile(path, 0);
        if (i >= 0) last = i;
    }
    if (last >= 0) selectLayer(last);
    e->acceptProposedAction();
}
