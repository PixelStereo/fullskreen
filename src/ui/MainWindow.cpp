#include "MainWindow.h"
#include "Commands.h"
#include "Engine.h"
#include "LayerInspector.h"
#include "LayerTable.h"
#include "MappingView.h"
#include "MasterPanel.h"
#include "MediaBin.h"
#include "OutputWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QUndoStack>
#include <QUrl>
#include <cmath>

static const QStringList kIsfExt = {"fs", "frag"};

static QScrollArea *scrolled(QWidget *w)
{
    auto *s = new QScrollArea;
    s->setWidget(w);
    s->setWidgetResizable(true);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->setFrameShape(QFrame::NoFrame);
    return s;
}

MainWindow::MainWindow(Engine *engine, QWidget *parent) : QMainWindow(parent), m_engine(engine)
{
    setAcceptDrops(true);
    resize(1600, 980);
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(500);

    QSettings s;
    m_engine->library().setUserFolders(s.value("isf/folders").toStringList());
    m_engine->library().scan();
    m_screenName = s.value("output/screen").toString();

    // --- Top: media bin | preview and mapping | Layer / Master tabs
    m_bin = new MediaBin(m_engine);
    m_bin->setMinimumWidth(260);
    m_view = new MappingView(m_engine);
    m_view->setUndoStack(m_undo);
    m_inspector = new LayerInspector(m_engine, m_undo);
    m_master = new MasterPanel(m_engine);
    m_tabs = new QTabWidget;
    m_tabs->addTab(scrolled(m_inspector), QStringLiteral("Layer"));
    m_tabs->addTab(scrolled(m_master), QStringLiteral("Master"));
    m_tabs->setMinimumWidth(390);

    auto *top = new QSplitter(Qt::Horizontal);
    top->addWidget(m_bin);
    top->addWidget(m_view);
    top->addWidget(m_tabs);
    top->setStretchFactor(0, 0);
    top->setStretchFactor(1, 1);
    top->setStretchFactor(2, 0);
    top->setSizes({330, 840, 430});

    // --- Bottom: layers across the full width
    m_layerTable = new LayerTable;
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(top);
    split->addWidget(m_layerTable);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setSizes({700, 240});
    split->setObjectName("mainSplit");
    top->setObjectName("topSplit");
    setCentralWidget(split);

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    // --- Output: the render thread presents the frame there itself
    m_output = new OutputWindow(m_engine);
    connect(m_output, &OutputWindow::closeRequested, this, [this] { setOutputMode(OutputHidden); });
    // When the output window has focus (click on the projector screen, or fullscreen on the main screen),
    // show-control shortcuts remain active.
    connect(m_output, &OutputWindow::keyPressed, this, [this](int key, Qt::KeyboardModifiers mods) { handleControlKey(key, mods); });

    buildMenus();

    // --- Layers
    connect(m_layerTable, &LayerTable::currentRowChanged, this, [this](int r) {
        m_view->setLayer(r);
        m_inspector->setLayer(r);
        if (r >= 0 && r != m_lastSelected) m_tabs->setCurrentIndex(0);
        m_lastSelected = r;
    });
    connect(m_layerTable, &LayerTable::visibilityToggled, this, [this](int row, bool on) {
        const QVariant before = cmd::SetLayerProp::read(m_engine, row, cmd::SetLayerProp::Visible);
        if (before.isValid() && before.toBool() != on)
            m_undo->push(new cmd::SetLayerProp(m_engine, row, cmd::SetLayerProp::Visible, before, on));
        if (row == m_inspector->layerIndex()) m_inspectorTimer.start();
    });
    connect(m_layerTable, &LayerTable::opacityEdited, this, [this](int row, double v) {
        const QVariant before = cmd::SetLayerProp::read(m_engine, row, cmd::SetLayerProp::Opacity);
        if (before.isValid() && std::abs(before.toDouble() - v) > 1e-6)
            m_undo->push(new cmd::SetLayerProp(m_engine, row, cmd::SetLayerProp::Opacity, before, v));
        if (row == m_inspector->layerIndex()) m_inspectorTimer.start(); // inspector resynced after the gesture
    });
    connect(m_layerTable, &LayerTable::removeClicked, this, &MainWindow::removeCurrentLayer);
    connect(m_layerTable, &LayerTable::duplicateClicked, this, &MainWindow::duplicateCurrentLayer);
    connect(m_layerTable, &LayerTable::moveClicked, this, &MainWindow::moveCurrentLayer);
    auto *del = new QShortcut(QKeySequence::Delete, m_layerTable->table(), nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &MainWindow::removeCurrentLayer);
    auto *backspace = new QShortcut(QKeySequence(Qt::Key_Backspace), m_layerTable->table(), nullptr, nullptr, Qt::WidgetShortcut);
    connect(backspace, &QShortcut::activated, this, &MainWindow::removeCurrentLayer);

    // --- Media bin
    connect(m_bin, &MediaBin::relinkRequested, this, &MainWindow::relinkMedia);
    connect(m_bin, &MediaBin::useAsSourceRequested, this, [this](const QString &p) { loadIntoLayer(currentLayer(), p); });
    connect(m_layerTable, &LayerTable::addClicked, this, &MainWindow::addEmptyLayer);
    connect(m_layerTable, &LayerTable::filesDropped, this, [this](int row, const QStringList &paths) {
        loadDropped(row >= 0 ? row : currentLayer(), paths);
    });
    connect(m_bin, &MediaBin::binEdited, this, &MainWindow::markDirty);
    m_binTimer.setSingleShot(true);
    m_binTimer.setInterval(150);
    connect(&m_binTimer, &QTimer::timeout, m_bin, &MediaBin::refresh);
    m_inspectorTimer.setSingleShot(true);
    m_inspectorTimer.setInterval(250);
    connect(&m_inspectorTimer, &QTimer::timeout, m_inspector, &LayerInspector::rebuild);

    // --- Master
    connect(m_master, &MasterPanel::blackoutChanged, this, [this](bool on) {
        QSignalBlocker b(m_blackoutAction);
        m_blackoutAction->setChecked(on);
    });
    connect(m_master, &MasterPanel::screenChosen, this, &MainWindow::chooseScreen);
    connect(m_master, &MasterPanel::fullscreenRequested, this, [this] { setOutputMode(OutputFullscreen); });
    connect(m_master, &MasterPanel::windowedRequested, this, [this] { setOutputMode(OutputWindowed); });
    connect(m_master, &MasterPanel::hideRequested, this, [this] { setOutputMode(OutputHidden); });
    connect(m_master, &MasterPanel::compositionEdited, this, &MainWindow::markDirty);
    connect(m_master, &MasterPanel::publishEdited, this, &MainWindow::markDirty);
    connect(m_master, &MasterPanel::audioEdited, this, &MainWindow::markDirty);
    connect(m_master, &MasterPanel::fitCompositionToScreenRequested, this, [this] {
        if (QScreen *sc = selectedScreen()) {
            const QSize px = sc->geometry().size() * sc->devicePixelRatio();
            if (px == m_engine->compositionSize()) return;
            m_engine->setCompositionSize(px);
            markDirty();
            statusBar()->showMessage(QStringLiteral("Composition: %1 × %2").arg(px.width()).arg(px.height()), 4000);
        }
    });

    // --- General sync
    connect(m_view, &MappingView::layerPicked, this, &MainWindow::selectLayer);
    connect(m_inspector, &LayerInspector::layerChanged, this, &MainWindow::refreshLayerList);
    connect(m_inspector, &LayerInspector::mappingChanged, m_view, qOverload<>(&QWidget::update));
    connect(m_inspector, &LayerInspector::fileDropped, this, [this](const QString &p) { loadIntoLayer(m_inspector->layerIndex(), p); });
    connect(m_engine, &Engine::layersChanged, this, &MainWindow::refreshLayerList);
    connect(m_engine, &Engine::compositionSizeChanged, this, [this] { m_view->update(); });
    // The preview follows rendering (at most one update per rendered frame)
    connect(m_engine, &Engine::frameRendered, this, [this] {
        m_engine->acknowledgeFrame();
        m_view->update();
    });
    // Rendering may have started before this connection: unblock the notification.
    m_engine->acknowledgeFrame();
    connect(m_undo, &QUndoStack::cleanChanged, this, [this] { updateTitle(); });
    connect(m_undo, &QUndoStack::indexChanged, this, [this] {
        refreshLayerList();   // in-place update (opacity, visibility, name…)
        m_binTimer.start();   // the media bin follows sources and shader images
    });

    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, &MainWindow::togglePlayCurrent);

    connect(qApp, &QGuiApplication::screenAdded, this, &MainWindow::buildOutputScreensMenu);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        buildOutputScreensMenu();
        if (m_outputMode != OutputHidden) setOutputMode(m_outputMode); // reposition
    });

    // UI: status and transport. Rendering itself does not depend on the UI.
    m_statusTimer.setInterval(100);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::statusTick);
    m_statusTimer.start();
    m_master->startAudio(); // sound card saved in the settings (system default otherwise)
    if (!m_engine->isThreaded()) {
        // Platform without OpenGL rendering on a separate thread: rendering is clocked by the UI.
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
    split->restoreState(s.value("ui/mainSplit").toByteArray());
    top->restoreState(s.value("ui/topSplit").toByteArray());
}

MainWindow::~MainWindow()
{
    m_statusTimer.stop();
    m_renderTimer.stop();
    m_autosaveTimer.stop();
    delete m_output;
}

// ---------------------------------------------------------------------------
// Show-control shortcuts
// ---------------------------------------------------------------------------

bool MainWindow::handleControlKey(int key, Qt::KeyboardModifiers mods)
{
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    if (ctrl && key == Qt::Key_B) setBlackout(!m_master->isBlackout());
    else if (ctrl && shift && key == Qt::Key_F) toggleWindowed();
    else if (ctrl && key == Qt::Key_F) toggleFullscreen();
    else if (!ctrl && key == Qt::Key_Space) togglePlayCurrent();
    else return false;
    return true;
}

void MainWindow::setBlackout(bool on)
{
    m_master->setBlackout(on);
    QSignalBlocker b(m_blackoutAction);
    m_blackoutAction->setChecked(on);
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("New Project"), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(QStringLiteral("Open…"), QKeySequence::Open, this, &MainWindow::openProjectDialog);
    file->addSeparator();
    file->addAction(QStringLiteral("Save"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(QStringLiteral("Save As…"), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addSeparator();
    file->addAction(QStringLiteral("Import to Media Bin…"), QKeySequence(Qt::CTRL | Qt::Key_I), this, [this] {
        QSettings s;
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import to Media Bin"),
                                                                s.value("dirs/video").toString(),
                                                                QStringLiteral("Videos, Images and Audio (*)"));
        if (!files.isEmpty()) m_bin->importFiles(files);
    });
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("Quit"), QKeySequence::Quit, this, &QWidget::close);
    quit->setMenuRole(QAction::QuitRole);

    // Undo / redo: custom actions so the whole UI refreshes afterwards
    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    m_undoAction = edit->addAction(QStringLiteral("Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = edit->addAction(QStringLiteral("Redo"));
    {
        // Ctrl+Shift+Z and Ctrl+Y everywhere (Cmd on Mac), without duplicating the native shortcut
        QList<QKeySequence> redo = QKeySequence::keyBindings(QKeySequence::Redo);
        for (const QKeySequence &k : {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)})
            if (!redo.contains(k)) redo << k;
        m_redoAction->setShortcuts(redo);
    }
    auto updateUndoActions = [this] {
        m_undoAction->setEnabled(m_undo->canUndo());
        m_redoAction->setEnabled(m_undo->canRedo());
        m_undoAction->setText(m_undo->canUndo() ? QStringLiteral("Undo %1").arg(m_undo->undoText())
                                                : QStringLiteral("Undo"));
        m_redoAction->setText(m_undo->canRedo() ? QStringLiteral("Redo %1").arg(m_undo->redoText())
                                                : QStringLiteral("Redo"));
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

    QMenu *layer = menuBar()->addMenu(QStringLiteral("&Layer"));
    // A layer is created empty; media is then dropped onto it (Media Bin, Finder)
    layer->addAction(QStringLiteral("New Layer"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this,
                     &MainWindow::addEmptyLayer);
    QAction *dup = layer->addAction(QStringLiteral("Duplicate"), this, &MainWindow::duplicateCurrentLayer);
    dup->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    layer->addAction(QStringLiteral("Delete (Del in list)"), this, &MainWindow::removeCurrentLayer);
    layer->addAction(QStringLiteral("Move Up"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                     [this] { moveCurrentLayer(-1); });
    layer->addAction(QStringLiteral("Move Down"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                     [this] { moveCurrentLayer(+1); });
    layer->addSeparator();
    layer->addAction(QStringLiteral("Play / Pause (Space)"), this, &MainWindow::togglePlayCurrent);

    QMenu *comp = menuBar()->addMenu(QStringLiteral("C&omposition"));
    comp->addAction(QStringLiteral("Settings (Master tab)"), this, [this] { m_tabs->setCurrentWidget(m_tabs->widget(1)); });
    QAction *outlines = comp->addAction(QStringLiteral("Show Outlines of Other Layers"));
    outlines->setCheckable(true);
    outlines->setChecked(true);
    connect(outlines, &QAction::toggled, m_view, &MappingView::setShowAllOutlines);

    QMenu *out = menuBar()->addMenu(QStringLiteral("&Output"));
    m_fullscreenAction = out->addAction(QStringLiteral("Fullscreen"));
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F));
    m_fullscreenAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_fullscreenAction, &QAction::triggered, this, [this] { toggleFullscreen(); });
    m_windowedAction = out->addAction(QStringLiteral("Windowed Output"));
    m_windowedAction->setCheckable(true);
    m_windowedAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    m_windowedAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_windowedAction, &QAction::triggered, this, [this] { toggleWindowed(); });
    out->addAction(QStringLiteral("Hide Output"), this, [this] { setOutputMode(OutputHidden); });
    out->addSeparator();
    m_blackoutAction = out->addAction(QStringLiteral("Blackout (Fade)"));
    m_blackoutAction->setCheckable(true);
    m_blackoutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    m_blackoutAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_blackoutAction, &QAction::toggled, this, &MainWindow::setBlackout);
    out->addSeparator();
    m_screensMenu = out->addMenu(QStringLiteral("Output Screen"));
    buildOutputScreensMenu();

    QMenu *lib = menuBar()->addMenu(QStringLiteral("ISF &Library"));
    lib->addAction(QStringLiteral("Add ISF Folder…"), this, &MainWindow::addIsfFolder);
    lib->addAction(QStringLiteral("Rescan"), QKeySequence::Refresh, this, &MainWindow::rescanLibrary);
    lib->addAction(QStringLiteral("Forget Added Folders"), this, [this] {
        m_engine->library().setUserFolders({});
        QSettings().setValue("isf/folders", QStringList());
        rescanLibrary();
    });
    lib->addSeparator();
    lib->addAction(QStringLiteral("Open Bundled Shaders Folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(IsfLibrary::bundledFolder()));
    });
    connect(lib, &QMenu::aboutToShow, this, [this, lib] {
        // Informational list of scanned folders
        for (QAction *a : lib->actions())
            if (a->property("folderInfo").toBool()) lib->removeAction(a), a->deleteLater();
        for (const QString &f : m_engine->library().allFolders()) {
            QAction *a = lib->addAction(f);
            a->setEnabled(false);
            a->setProperty("folderInfo", true);
        }
    });
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void MainWindow::buildOutputScreensMenu()
{
    m_screensMenu->clear();
    delete m_screenGroup;
    m_screenGroup = new QActionGroup(this);
    const auto screens = QGuiApplication::screens();
    bool found = false;
    for (QScreen *sc : screens) found |= sc->name() == m_screenName;
    if (!found) {
        // Default: the first screen that is not the main screen (projector), otherwise the main screen
        QScreen *primary = QGuiApplication::primaryScreen();
        m_screenName = primary ? primary->name() : QString();
        for (QScreen *sc : screens)
            if (sc != primary) {
                m_screenName = sc->name();
                break;
            }
    }
    QList<QPair<QString, QString>> list;
    for (QScreen *sc : screens) {
        const QSize px = sc->geometry().size() * sc->devicePixelRatio();
        const QString label = QStringLiteral("%1  (%2 × %3)%4")
                                  .arg(sc->name())
                                  .arg(px.width())
                                  .arg(px.height())
                                  .arg(sc == QGuiApplication::primaryScreen() ? QStringLiteral(" — main") : QString());
        list << qMakePair(label, sc->name());
        QAction *a = m_screensMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(sc->name() == m_screenName);
        m_screenGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, name = sc->name()] { chooseScreen(name); });
    }
    m_master->setScreens(list, m_screenName);
}

void MainWindow::chooseScreen(const QString &name)
{
    m_screenName = name;
    QSettings().setValue("output/screen", name);
    buildOutputScreensMenu();
    if (m_outputMode != OutputHidden) setOutputMode(m_outputMode);
}

QScreen *MainWindow::selectedScreen() const
{
    for (QScreen *sc : QGuiApplication::screens())
        if (sc->name() == m_screenName) return sc;
    return QGuiApplication::primaryScreen();
}

void MainWindow::toggleFullscreen()
{
    setOutputMode(m_outputMode == OutputFullscreen ? OutputHidden : OutputFullscreen);
}

void MainWindow::toggleWindowed()
{
    setOutputMode(m_outputMode == OutputWindowed ? OutputHidden : OutputWindowed);
}

void MainWindow::setOutputMode(OutputMode mode)
{
    m_outputMode = mode;
    m_fullscreenAction->setChecked(mode == OutputFullscreen);
    m_windowedAction->setChecked(mode == OutputWindowed);
    m_master->setOutputMode(int(mode));
    m_autosaveDone = false; // output state is part of the session to restore
    if (mode == OutputHidden) {
        m_output->hideOutput();
        activateWindow();
        return;
    }
    QScreen *sc = selectedScreen();
    const bool sameAsUi = sc == screen();
    m_output->showOn(sc, mode == OutputFullscreen);
    if (mode == OutputFullscreen && sameAsUi) {
        // Fullscreen on the UI screen: the output comes to the front and keeps the keyboard (⌘F / Ctrl+F to exit)
        m_output->raise();
        m_output->requestActivate();
        statusBar()->showMessage(QStringLiteral("Fullscreen on main screen: Ctrl+F (⌘F) to return."), 8000);
    } else {
        activateWindow();
    }
}

// ---------------------------------------------------------------------------
// Layer list
// ---------------------------------------------------------------------------

static QString layerTag(SourceType t)
{
    switch (t) {
    case SourceType::Video: return QStringLiteral("▶");
    case SourceType::Image: return QStringLiteral("▣");
    case SourceType::Isf: return QStringLiteral("◆");
    case SourceType::Audio: return QStringLiteral("♪");
    default: return QStringLiteral("○");
    }
}

static QString fmtClock(double s)
{
    if (s < 0) s = 0;
    const int m = int(s) / 60;
    return QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(s - m * 60, 5, 'f', 2, QChar('0'));
}

void MainWindow::refreshLayerList()
{
    std::vector<LayerTable::Row> rows;
    {
        Engine::Lock lk(&m_engine->mutex());
        for (int i = 0; i < m_engine->layerCount(); ++i) {
            Layer *l = m_engine->layer(i);
            LayerTable::Row r;
            r.name = l->name;
            r.tag = layerTag(l->type != SourceType::None ? l->type : l->missingType);
            r.visible = l->visible;
            r.opacity = l->opacity;
            r.blend = blendModeName(l->blend);
            r.error = !l->error.isEmpty();
            r.noPicture = l->type == SourceType::Audio || (l->type == SourceType::None && l->missingType == SourceType::Audio);
            switch (l->type) {
            case SourceType::Video:
            case SourceType::Audio:
            case SourceType::Image: r.source = QFileInfo(l->sourcePath).fileName(); break;
            case SourceType::Isf: r.source = QStringLiteral("generator ") + QFileInfo(l->sourcePath).completeBaseName(); break;
            default: r.source = l->missingType != SourceType::None ? QFileInfo(l->sourcePath).fileName() + QStringLiteral(" — missing")
                                                                    : QStringLiteral("(empty)");
            }
            QStringList fx;
            for (const auto &e : l->effects) fx << (e->enabled ? e->name() : QStringLiteral("(") + e->name() + QStringLiteral(")"));
            r.effects = fx.join(QStringLiteral(" › "));
            if (l->audio) {
                const QString vol = l->muted ? QStringLiteral("muted") : QStringLiteral("%1%").arg(std::lround(l->volume * 100));
                r.source += (l->type == SourceType::Video ? QStringLiteral("   ♪ ") : QStringLiteral("   ")) + vol;
            }
            if (l->type == SourceType::Video || l->type == SourceType::Audio) {
                r.playback = QStringLiteral("%1 %2 / %3").arg(l->playing ? QStringLiteral("▶") : QStringLiteral("❚❚"),
                                                               fmtClock(l->position()), fmtClock(l->duration()));
            } else if (l->type == SourceType::Isf) {
                r.playback = QStringLiteral("real time");
            }
            rows.push_back(r);
        }
    }
    const int n = int(rows.size());
    const int keep = qBound(-1, m_layerTable->currentRow(), n - 1);
    m_layerTable->setRows(rows);
    const int sel = keep >= 0 ? keep : (n > 0 ? 0 : -1);
    if (sel != m_layerTable->currentRow()) m_layerTable->setCurrentRow(sel);
    if (sel != m_inspector->layerIndex()) {
        m_inspector->setLayer(sel);
        m_view->setLayer(sel);
        m_lastSelected = sel;
    }
}

void MainWindow::refreshAll()
{
    refreshLayerList();
    m_inspector->rebuild();
    m_view->update();
    m_bin->refresh();
}

void MainWindow::selectLayer(int index)
{
    refreshLayerList();
    m_layerTable->setCurrentRow(index);
    m_view->setLayer(index);
    m_inspector->setLayer(index);
    if (index >= 0 && index != m_lastSelected) m_tabs->setCurrentIndex(0);
    m_lastSelected = index;
}

int MainWindow::currentLayer() const { return m_layerTable->currentRow(); }

// ---------------------------------------------------------------------------
// Layer actions (all undoable)
// ---------------------------------------------------------------------------

bool MainWindow::loadIntoLayer(int i, const QString &path)
{
    if (i < 0 || i >= m_engine->layerCount()) {
        statusBar()->showMessage(QStringLiteral("Create a layer with + first, then drop the media onto it."), 6000);
        return false;
    }
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    auto fail = [&](const QString &err) {
        const QString msg = fi.fileName() + QStringLiteral(": ") + err.section('\n', 0, 0);
        if (m_quiet) statusBar()->showMessage(msg, 8000);
        else QMessageBox::warning(this, QStringLiteral("Load"), msg);
        return false;
    };
    if (kIsfExt.contains(ext)) {
        const IsfInstance::Header h = IsfInstance::readHeader(path);
        if (!h.ok) return fail(QStringLiteral("not a valid ISF shader."));
        if (h.isFilter) { // an ISF effect joins the layer's effect chain
            const QJsonArray before = m_engine->effectsJson(i);
            m_engine->addEffect(i, path);
            m_undo->push(new cmd::SetEffects(m_engine, i, before, QStringLiteral("Add Effect %1").arg(fi.completeBaseName())));
            selectLayer(i);
            refreshAll();
            return true;
        }
    }
    const QJsonObject before = m_engine->layerJson(i);
    QString err;
    if (kIsfExt.contains(ext)) m_engine->setLayerIsf(i, path, &err); // a broken shader still loads (error shown)
    else if (!m_engine->setLayerFile(i, path, &err)) return fail(err);
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(i);
        if (l && l->name.startsWith(QStringLiteral("Layer "))) l->name = fi.completeBaseName();
    }
    m_undo->push(new cmd::ReplaceLayer(m_engine, i, before, QStringLiteral("Load \"%1\"").arg(fi.fileName())));
    selectLayer(i);
    refreshAll();
    if (!err.isEmpty()) statusBar()->showMessage(fi.fileName() + QStringLiteral(": ") + err.section('\n', 0, 0), 8000);
    return true;
}

void MainWindow::loadDropped(int layer, const QStringList &paths)
{
    // The first source file is loaded into the layer, ISF effects join its chain,
    // the other media go to the Media Bin (only "+" creates layers).
    bool sourceLoaded = false;
    QStringList toBin;
    for (const QString &p : paths) {
        const QString ext = QFileInfo(p).suffix().toLower();
        const bool effect = kIsfExt.contains(ext) && IsfInstance::readHeader(p).isFilter;
        if (effect) {
            loadIntoLayer(layer, p);
        } else if (!sourceLoaded) {
            sourceLoaded = loadIntoLayer(layer, p);
            if (!kIsfExt.contains(ext)) toBin << p;
        } else if (!kIsfExt.contains(ext)) {
            toBin << p;
        }
    }
    if (toBin.size() > 1 || (!sourceLoaded && !toBin.isEmpty())) {
        m_bin->importFiles(toBin);
        if (toBin.size() > 1) statusBar()->showMessage(QStringLiteral("%1 file(s) added to the Media Bin").arg(toBin.size()), 5000);
    }
}

void MainWindow::relinkMedia(const QString &from, const QString &to)
{
    const QList<int> layers = m_engine->layersUsingMedia(from);
    QStringList errors;
    if (!layers.isEmpty()) {
        // A single undo step for all affected layers
        m_undo->beginMacro(QStringLiteral("Relink \"%1\"").arg(QFileInfo(from).fileName()));
        for (int i : layers) {
            const QJsonObject before = m_engine->layerJson(i);
            QString err;
            if (m_engine->relinkLayerMedia(i, from, to, &err))
                m_undo->push(new cmd::ReplaceLayer(m_engine, i, before, QStringLiteral("Relink File")));
            else if (!err.isEmpty()) errors << err;
        }
        m_undo->endMacro();
    }
    m_engine->relinkBinItem(from, to);
    markDirty();
    refreshAll();
    if (!errors.isEmpty()) QMessageBox::warning(this, QStringLiteral("Relink"), errors.join('\n'));
    else statusBar()->showMessage(QStringLiteral("\"%1\" relinked in %2 layer(s)").arg(QFileInfo(from).fileName()).arg(layers.size()), 5000);
}

void MainWindow::addEmptyLayer()
{
    int i = m_engine->addLayer();
    m_undo->push(new cmd::AddLayer(m_engine, i, QStringLiteral("Add Layer")));
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
    m_undo->push(new cmd::AddLayer(m_engine, ni, QStringLiteral("Duplicate Layer")));
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
        if (!l || !l->hasTransport()) return;
        playing = l->playing;
    }
    m_engine->setLayerPlaying(i, !playing);
    m_inspector->refreshDynamic();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void MainWindow::statusTick()
{
    m_engine->acknowledgeFrame(); // safety net: the preview cannot stay frozen
    m_inspector->refreshDynamic();
    m_master->refreshStatus();
    refreshLayerList(); // playback positions (in-place update)
    const int pct = int(std::lround(m_engine->masterLevel() * 100));
    const QSize c = m_engine->compositionSize();
    QStringList pubs;
    for (int k = 0; k < kPublishKindCount; ++k)
        if (m_engine->publishState(PublishKind(k)).level == PublishState::Ok) pubs << publishKindName(PublishKind(k));
    const QString mode = m_outputMode == OutputFullscreen ? QStringLiteral("fullscreen ") + m_screenName
                         : m_outputMode == OutputWindowed ? QStringLiteral("windowed")
                                                          : QStringLiteral("hidden");
    m_status->setText(QStringLiteral("%1 × %2   ·   %3 fps   ·   master %4%   ·   output: %5%6")
                          .arg(c.width())
                          .arg(c.height())
                          .arg(m_engine->fps(), 0, 'f', 1)
                          .arg(pct)
                          .arg(mode, pubs.isEmpty() ? QString() : QStringLiteral("   ·   published: ") + pubs.join(", ")));
}

// ---------------------------------------------------------------------------
// ISF Library
// ---------------------------------------------------------------------------

void MainWindow::addIsfFolder()
{
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("ISF Shader Folder"));
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
    m_bin->refresh();
    m_inspector->rebuild();
    statusBar()->showMessage(QStringLiteral("ISF Library: %1 generators, %2 effects")
                                 .arg(m_engine->library().generators().size())
                                 .arg(m_engine->library().filters().size()),
                             5000);
}

// ---------------------------------------------------------------------------
// Project, autosave, recovery
// ---------------------------------------------------------------------------

bool MainWindow::isDirty() const { return m_forceDirty || !m_undo->isClean(); }

void MainWindow::markDirty()
{
    m_forceDirty = true;
    m_autosaveDone = false;
    updateTitle();
}

void MainWindow::updateTitle()
{
    const QString p = m_engine->projectPath();
    setWindowTitle((p.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(p).fileName()) + QStringLiteral("[*] — Fulskrin"));
    setWindowModified(isDirty());
}

QJsonObject MainWindow::uiState() const
{
    QJsonObject o;
    o["outputScreen"] = m_screenName;
    o["selectedLayer"] = currentLayer();
    o["outputMode"] = m_outputMode == OutputFullscreen ? "fullscreen" : m_outputMode == OutputWindowed ? "window" : "hidden";
    return o;
}

bool MainWindow::maybeSave()
{
    if (!isDirty()) return true;
    auto r = QMessageBox::question(this, QStringLiteral("Fulskrin"), QStringLiteral("Save changes to the project?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::afterProjectLoaded(const QJsonObject &ui)
{
    m_undo->clear();
    m_autosaveDone = false;
    const QString scr = ui.value("outputScreen").toString();
    if (!scr.isEmpty()) m_screenName = scr;
    buildOutputScreensMenu();
    m_master->syncFromEngine();
    m_bin->refresh();
    selectLayer(qBound(-1, ui.value("selectedLayer").toInt(0), m_engine->layerCount() - 1));
    updateTitle();
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_engine->newProject();
    m_forceDirty = false;
    afterProjectLoaded({});
}

void MainWindow::openProjectDialog()
{
    if (!maybeSave()) return;
    QSettings s;
    const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Open Project"), s.value("dirs/project").toString(),
                                                   QStringLiteral("Fulskrin Projects (*.fulskrin *.json)"));
    if (!f.isEmpty()) openProject(f);
}

bool MainWindow::openProject(const QString &path)
{
    QJsonObject ui;
    QString err;
    if (!m_engine->loadProject(path, &ui, &err) && !err.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Open Project"), err);
        return false;
    }
    m_forceDirty = false;
    afterProjectLoaded(ui);
    QSettings s;
    s.setValue("dirs/project", QFileInfo(path).absolutePath());
    s.setValue("project/last", QFileInfo(path).absoluteFilePath());
    if (!err.isEmpty() && m_quiet) statusBar()->showMessage(err.section('\n', 0, 0), 15000);
    else if (!err.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Open Project"),
                             QStringLiteral("Project opened with warnings:\n\n") + err
                                 + QStringLiteral("\n\nMissing files are shown in red in the Media Bin: "
                                                  "use \"Replace…\" to relink them."));
    return true;
}

bool MainWindow::saveTo(const QString &path)
{
    QString err;
    if (!m_engine->saveProject(path, uiState(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Save"), err);
        return false;
    }
    m_engine->setProjectPath(QFileInfo(path).absoluteFilePath());
    m_undo->setClean();
    m_forceDirty = false;
    updateTitle();
    statusBar()->showMessage(QStringLiteral("Saved: ") + path, 4000);
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
    QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save Project"), s.value("dirs/project").toString(),
                                             QStringLiteral("Fulskrin Projects (*.fulskrin)"));
    if (f.isEmpty()) return false;
    if (QFileInfo(f).suffix().isEmpty()) f += ".fulskrin";
    if (!saveTo(f)) return false;
    s.setValue("dirs/project", QFileInfo(f).absolutePath());
    s.setValue("project/last", f);
    return true;
}

QString MainWindow::autosavePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/autosave.fulskrin";
}

void MainWindow::autosave()
{
    if (!m_autosaveEnabled) return;
    if (m_autosaveDone && m_undo->index() == m_autosaveIndex) return; // nothing new
    QJsonObject ui = uiState();
    ui["autosaveOf"] = m_engine->projectPath();
    ui["autosaveTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    ui["autosaveDirty"] = isDirty();
    QString err;
    if (m_engine->saveProject(autosavePath(), ui, &err)) {
        m_autosaveIndex = m_undo->index();
        m_autosaveDone = true;
    } else {
        statusBar()->showMessage(QStringLiteral("Autosave failed: ") + err, 6000);
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
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Recovery"), QStringLiteral("Fulskrin did not quit normally."),
                    QMessageBox::NoButton, this);
    box.setInformativeText(QStringLiteral("Restore the session autosaved on %1 %2?")
                               .arg(when.isValid() ? when.toString("dd/MM 'at' HH:mm:ss") : QStringLiteral("(unknown date)"),
                                    original.isEmpty() ? QString() : QStringLiteral("(project %1)").arg(QFileInfo(original).fileName())));
    QPushButton *restore = box.addButton(QStringLiteral("Restore"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Ignore"), QMessageBox::RejectRole);
    box.setDefaultButton(restore);
    box.exec();
    if (box.clickedButton() != restore) {
        QFile::remove(path);
        return;
    }
    QJsonObject loadedUi;
    QString err;
    m_engine->loadProject(path, &loadedUi, &err);
    m_engine->setProjectPath(original); // "Save" writes to the original project
    m_forceDirty = ui.value("autosaveDirty").toBool(true);
    afterProjectLoaded(loadedUi);
    if (!err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Recovery"), err);
    QString mode = ui.value("outputMode").toString();
    if (mode.isEmpty() && ui.value("outputVisible").toBool()) mode = "fullscreen"; // older sessions
    if (mode == "fullscreen" || mode == "window") {
        // Output comes back, but blacked out: the operator decides when to bring it back up.
        m_engine->fadeMaster(0.0, 0.0);
        setBlackout(true);
        setOutputMode(mode == "fullscreen" ? OutputFullscreen : OutputWindowed);
        statusBar()->showMessage(QStringLiteral("Session restored — output blacked out: Ctrl+B (⌘B) to bring it back"), 15000);
    } else {
        statusBar()->showMessage(QStringLiteral("Session restored"), 6000);
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings s;
    s.setValue("ui/geometry", saveGeometry());
    if (auto *split = findChild<QSplitter *>("mainSplit")) s.setValue("ui/mainSplit", split->saveState());
    if (auto *top = findChild<QSplitter *>("topSplit")) s.setValue("ui/topSplit", top->saveState());
    m_output->hideOutput();
    // Normal exit: no recovery to offer on next launch.
    if (m_autosaveEnabled) QFile::remove(autosavePath());
    e->accept();
}

// ---------------------------------------------------------------------------
// Drag and drop (files from the system or the media bin)
// ---------------------------------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    // Elsewhere than on a layer (preview, empty area): loaded into the selected layer
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls()) {
        const QString path = u.toLocalFile();
        if (path.isEmpty()) continue;
        if (QFileInfo(path).suffix().toLower() == "fulskrin") {
            if (maybeSave()) openProject(path);
            e->acceptProposedAction();
            return;
        }
        paths << path;
    }
    if (!paths.isEmpty()) loadDropped(currentLayer(), paths);
    e->acceptProposedAction();
}
