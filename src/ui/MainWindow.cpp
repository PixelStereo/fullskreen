#include "MainWindow.h"
#include "Commands.h"
#include "Engine.h"
#include "LayerInspector.h"
#include "LayerTable.h"
#include "MappingView.h"
#include "CompositionPanel.h" // CompositionPanel
#include "MediaBin.h"
#include "SnapshotPanel.h"
#include "SequencePanel.h"
#include "TimelinePanel.h"
#include "OutputWindow.h"
#include "Osc.h"
#include "SettingsPanel.h"
#include "Widgets.h"

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
#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QAbstractSpinBox>
#include <QLabel>
#include <QLineEdit>
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
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QUndoStack>
#include <QVBoxLayout>
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

    // --- Top: media bin | preview and mapping | Layer / Composition tabs
    m_bin = new MediaBin(m_engine);
    m_bin->setMinimumWidth(260);
    m_view = new MappingView(m_engine);
    m_view->setUndoStack(m_undo);
    m_inspector = new LayerInspector(m_engine, m_undo);
    m_composition = new CompositionPanel(m_engine);
    m_tabs = new QTabWidget;
    m_tabs->addTab(scrolled(m_inspector), QStringLiteral("Layer"));
    m_tabs->addTab(scrolled(m_composition), QStringLiteral("Composition"));
    m_settings = new SettingsPanel(m_engine);
    m_tabs->addTab(scrolled(m_settings), QStringLiteral("Settings"));
    connect(m_settings, &SettingsPanel::playModeChanged, this,
            [this] { m_engine->setDefaultPlayMode(SettingsPanel::defaultPlayMode()); });
    VideoDecoder::setHardwareDecoding(SettingsPanel::hardwareDecoding());
    connect(m_settings, &SettingsPanel::hardwareDecodingChanged, this,
            [] { VideoDecoder::setHardwareDecoding(SettingsPanel::hardwareDecoding()); });
    // For the layers created from now on; each layer keeps its own choice
    connect(m_settings, &SettingsPanel::colorModelsChanged, this,
            [this] { m_engine->setDefaultColorModels(SettingsPanel::colorModels()); });
    m_engine->setDefaultColorModels(SettingsPanel::colorModels());
    // Rendering: the machine's defaults, and the main screen's refresh rate (the pace without an output shown)
    m_engine->setRenderDefaults(SettingsPanel::renderDefaults());
    auto screenRate = [this] {
        if (QScreen *sc = QGuiApplication::primaryScreen()) m_engine->setScreenRefreshRate(sc->refreshRate());
    };
    screenRate();
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, screenRate);
    connect(m_settings, &SettingsPanel::oscChanged, this, &MainWindow::startOsc);
    // Default transition of the sources changed by a snapshot
    m_engine->setDefaultTransition(SettingsPanel::defaultTransition(m_engine->library()));
    m_settings->setTransitions(m_engine->library().transitions(), m_engine->defaultTransition());
    connect(m_settings, &SettingsPanel::transitionChanged, this, [this](const QString &p) { m_engine->setDefaultTransition(p); });
    m_tabs->setMinimumWidth(390);

    // Left: Media Bin and Layers, two tabs (Shift+1 / Shift+2). Dragging a media over the Layers tab opens it.
    m_layerTable = new LayerTable;
    m_leftTabs = new QTabWidget;
    m_leftTabs->addTab(m_bin, QStringLiteral("Media Bin"));
    m_leftTabs->addTab(m_layerTable, QStringLiteral("Layers"));
    m_leftTabs->setTabToolTip(0, QStringLiteral("Media Bin (Shift+1)"));
    m_leftTabs->setTabToolTip(1, QStringLiteral("Layers (Shift+2)"));
    m_leftTabs->tabBar()->setChangeCurrentOnDrag(true);
    m_leftTabs->tabBar()->setAcceptDrops(true);
    m_leftTabs->setCurrentIndex(1);

    auto *top = new QSplitter(Qt::Horizontal);
    top->addWidget(m_leftTabs);
    // The preview, with the sequences' bar under it
    m_seqBar = new SequenceBar(m_engine);
    auto *center = new QWidget;
    auto *cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    cv->addWidget(m_view, 1);
    cv->addWidget(m_seqBar);
    top->addWidget(center);
    top->addWidget(m_tabs);
    top->setStretchFactor(0, 0);
    top->setStretchFactor(1, 1);
    top->setStretchFactor(2, 0);
    top->setSizes({560, 610, 430});

    // --- Bottom: snapshots (grid + inspector)
    m_snapshots = new SnapshotPanel(m_engine, m_undo);
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(top);
    split->addWidget(m_snapshots);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setSizes({720, 220});
    split->setObjectName("mainSplit2"); // (layout changed: earlier saved states are not restored)
    top->setObjectName("topSplit2");
    connect(m_snapshots, &SnapshotPanel::edited, this, &MainWindow::markDirty);
    connect(m_snapshots, &SnapshotPanel::recalled, this, [this] { refreshAll(); });
    qApp->installEventFilter(this); // Shift+1 / Shift+2: left tabs
    setCentralWidget(split);

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    buildMenus();
    m_engine->setDefaultPlayMode(SettingsPanel::defaultPlayMode());

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
        if (refuseLocked(row)) return refreshLayerList();
        const QVariant before = cmd::SetLayerProp::read(m_engine, row, cmd::SetLayerProp::Opacity);
        if (before.isValid() && std::abs(before.toDouble() - v) > 1e-6)
            m_undo->push(new cmd::SetLayerProp(m_engine, row, cmd::SetLayerProp::Opacity, before, v));
        if (row == m_inspector->layerIndex()) m_inspectorTimer.start(); // inspector resynced after the gesture
    });
    connect(m_layerTable, &LayerTable::removeClicked, this, &MainWindow::removeCurrentLayer);
    connect(m_layerTable, &LayerTable::groupClicked, this, &MainWindow::createGroup);
    connect(m_layerTable, &LayerTable::viewportClicked, this, &MainWindow::addViewport);
    connect(m_layerTable, &LayerTable::lockToggled, this, [this](int row) {
        if (m_engine->groupIndexOf(row) >= 0 && m_engine->isLocked(m_engine->groupIndexOf(row))) { // any group above
            statusBar()->showMessage(QStringLiteral("Locked by its group: unlock the group first."), 4000);
            return;
        }
        const QVariant before = cmd::SetLayerProp::read(m_engine, row, cmd::SetLayerProp::Locked);
        if (!before.isValid()) return;
        m_undo->push(new cmd::SetLayerProp(m_engine, row, cmd::SetLayerProp::Locked, before, !before.toBool()));
        m_inspector->rebuild();
    });
    connect(m_layerTable, &LayerTable::collapseToggled, this, [this](int row) {
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(row);
            if (!l || !l->isGroup) return;
            l->collapsed = !l->collapsed;
        }
        markDirty(); // saved in the project, not an undoable edit
        // The selection does not stay on a hidden row
        bool inside = false;
        for (int g = m_engine->groupIndexOf(currentLayer()); g >= 0 && !inside; g = m_engine->groupIndexOf(g)) inside = g == row;
        if (inside) selectLayer(row);
        else refreshLayerList();
    });
    connect(m_layerTable, &LayerTable::renamed, this, [this](int row, const QString &name) {
        if (refuseLocked(row)) return;
        const QVariant before = cmd::SetLayerProp::read(m_engine, row, cmd::SetLayerProp::Name);
        if (before.isValid() && before.toString() != name)
            m_undo->push(new cmd::SetLayerProp(m_engine, row, cmd::SetLayerProp::Name, before, name));
        refreshLayerList();
        if (row == m_inspector->layerIndex()) m_inspector->rebuild();
    });
    connect(m_layerTable, &LayerTable::moveRequested, this, &MainWindow::moveRows);
    connect(m_layerTable, &LayerTable::duplicateClicked, this, &MainWindow::duplicateCurrentLayer);
    connect(m_layerTable, &LayerTable::contextMenuRequested, this, &MainWindow::layerContextMenu);
    connect(m_layerTable, &LayerTable::moveClicked, this, &MainWindow::moveCurrentLayer);
    auto *del = new QShortcut(QKeySequence::Delete, m_layerTable->table(), nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &MainWindow::removeCurrentLayer);
    auto *backspace = new QShortcut(QKeySequence(Qt::Key_Backspace), m_layerTable->table(), nullptr, nullptr, Qt::WidgetShortcut);
    connect(backspace, &QShortcut::activated, this, &MainWindow::removeCurrentLayer);

    // --- Media bin
    connect(m_bin, &MediaBin::relinkRequested, this, &MainWindow::relinkMedia);
    connect(m_bin, &MediaBin::useAsSourceRequested, this, [this](const QString &p) { loadIntoLayer(currentLayer(), p); });
    connect(m_bin, &MediaBin::loadIntoNewLayerRequested, this, &MainWindow::loadIntoNewLayer);
    connect(m_bin, &MediaBin::loadIntoLayerRequested, this, [this](int i, const QString &p) { loadIntoLayer(i, p); });
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

    // --- Composition
    connect(m_composition, &CompositionPanel::blackoutChanged, this, [this](bool on) {
        QSignalBlocker b(m_blackoutAction);
        m_blackoutAction->setChecked(on);
    });
    connect(m_composition, &CompositionPanel::compositionEdited, this, &MainWindow::markDirty);
    connect(m_settings, &SettingsPanel::renderDefaultsChanged, this, [this] {
        m_engine->setRenderDefaults(SettingsPanel::renderDefaults());
    });
    connect(m_composition, &CompositionPanel::audioEdited, this, &MainWindow::markDirty);

    // --- General sync
    connect(m_view, &MappingView::layerPicked, this, &MainWindow::selectLayer);
    connect(m_inspector, &LayerInspector::layerChanged, this, &MainWindow::refreshLayerList);
    connect(m_inspector, &LayerInspector::projectEdited, this, &MainWindow::markDirty);
    connect(m_inspector, &LayerInspector::rescanLibraryRequested, this, &MainWindow::rescanLibrary,
            Qt::QueuedConnection); // the rebuild destroys the button that emitted it
    connect(m_inspector, &LayerInspector::kindChanged, this, [this](const QString &k) { m_tabs->setTabText(0, k); });
    connect(m_inspector, &LayerInspector::mappingChanged, m_view, qOverload<>(&QWidget::update));
    connect(m_inspector, &LayerInspector::fileDropped, this, [this](const QString &p) { loadIntoLayer(m_inspector->layerIndex(), p); });
    // The accent color is a machine preference: applied at once, everywhere
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) theme::applyToApplication(*app);
        refreshAll();
        update();
    });
    connect(m_engine, &Engine::layersChanged, this, &MainWindow::refreshLayerList);
    connect(m_engine, &Engine::layersChanged, this, &MainWindow::syncOutputs); // windows follow the viewports
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

    // I / O: in / out point of the selected video or sound at its current position
    auto *keyIn = new QShortcut(QKeySequence(Qt::Key_I), this);
    connect(keyIn, &QShortcut::activated, this, [this] { setInOutAtPosition(true); });
    auto *keyOut = new QShortcut(QKeySequence(Qt::Key_O), this);
    connect(keyOut, &QShortcut::activated, this, [this] { setInOutAtPosition(false); });
    connect(m_inspector, &LayerInspector::setInOutRequested, this, &MainWindow::setInOutAtPosition);
    // Space: GO, the next step of the sequence (also from the sequences' window and the outputs)
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this, nullptr, nullptr, Qt::ApplicationShortcut);
    connect(space, &QShortcut::activated, this, &MainWindow::sequenceGo);
    auto *spaceBack = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Space), this, nullptr, nullptr, Qt::ApplicationShortcut);
    connect(spaceBack, &QShortcut::activated, this, &MainWindow::sequenceBack);
    m_seqWindow = new SequenceWindow(m_engine, this);
    connect(m_seqBar, &SequenceBar::goRequested, this, &MainWindow::sequenceGo);
    connect(m_seqBar, &SequenceBar::backRequested, this, &MainWindow::sequenceBack);
    connect(m_seqBar, &SequenceBar::windowRequested, this, &MainWindow::openSequences);
    connect(m_seqWindow, &SequenceWindow::goToRequested, this, &MainWindow::sequenceGoTo);
    connect(m_seqWindow, &SequenceWindow::edited, this, &MainWindow::markDirty);
    connect(m_seqBar, &SequenceBar::stopRequested, m_engine, &Engine::sequenceStop);
    connect(m_seqWindow, &SequenceWindow::stopRequested, m_engine, &Engine::sequenceStop);
    m_timelineWindow = new TimelineWindow(m_engine, m_undo, this);
    m_timelineWindow->addActions({m_undoAction, m_redoAction}); // undo / redo from the Timelines window too
    connect(m_timelineWindow, &TimelineWindow::edited, this, &MainWindow::markDirty);
    connect(m_seqBar, &SequenceBar::timelinesRequested, this, &MainWindow::openTimelines);
    connect(m_seqWindow, &SequenceWindow::timelinesRequested, this, &MainWindow::openTimelines);
    connect(m_layerTable, &LayerTable::currentRowChanged, this, [this](int r) {
        m_timelineWindow->setCurrentLayer(r >= 0 ? m_engine->layerId(r) : 0);
    });
    // A step's snapshot, when its pre-wait is over (GO, Space, OSC, or a step that follows): undoable; the bar
    // then shows the step unchanged
    m_engine->setRecaller([this](int snapshot) {
        m_snapshots->recall(snapshot);
        m_cueUndoIndex = m_undo->index();
        m_seqBar->setModified(false);
        m_seqWindow->setModified(false);
    });
    // Played elsewhere (OSC) or another sequence: nothing changed since; any edit afterwards: the bar says so
    connect(m_engine, &Engine::sequencePositionChanged, this, [this] {
        m_cueUndoIndex = m_undo->index();
        m_seqBar->setModified(false);
        m_seqWindow->setModified(false);
    });
    connect(m_undo, &QUndoStack::indexChanged, this, [this](int i) {
        if (m_engine->sequencePosition() >= 0 && i != m_cueUndoIndex) {
            m_seqBar->setModified(true);
            m_seqWindow->setModified(true);
        }
    });

    // A screen plugged in or out: the outputs go back to their screens (or to another one meanwhile),
    // and the Output tab lists the screens again
    auto screensChanged = [this] {
        syncOutputs();
        m_inspectorTimer.start();
    };
    connect(qApp, &QGuiApplication::screenAdded, this, screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this, screensChanged] {
        QTimer::singleShot(0, this, screensChanged); // once the screen is gone from the list
    });

    // UI: status and transport. Rendering itself does not depend on the UI.
    m_statusTimer.setInterval(100);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::statusTick);
    m_statusTimer.start();
    m_settings->startAudio(); // sound card saved in the settings (system default otherwise)
    if (!m_engine->isThreaded()) {
        // Platform without OpenGL rendering on a separate thread: rendering is clocked by the UI.
        m_renderTimer.setTimerType(Qt::PreciseTimer);
        m_renderTimer.setInterval(16);
        // The rate chosen, or the screen's
        auto *pace = new QTimer(this);
        connect(pace, &QTimer::timeout, this, [this] {
            const double r = m_engine->effectiveRender().frameRate;
            const int ms = int(1000.0 / std::clamp(r > 0 ? r : m_engine->screenRefreshRate(), 1.0, 1000.0));
            if (m_renderTimer.interval() != ms) m_renderTimer.setInterval(ms);
        });
        pace->start(500);
        connect(&m_renderTimer, &QTimer::timeout, m_engine, &Engine::renderFrame);
        m_renderTimer.start();
    }
    m_autosaveTimer.setInterval(10000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer.start();

    refreshLayerList();
    syncOutputs();
    updateTitle();
    startOsc();
    restoreGeometry(s.value("ui/geometry").toByteArray());
    split->restoreState(s.value("ui/mainSplit2").toByteArray());
    top->restoreState(s.value("ui/topSplit2").toByteArray());
}

MainWindow::~MainWindow()
{
    m_engine->setRecaller(nullptr);
    if (m_oscThread) {
        QMetaObject::invokeMethod(m_osc, [this] { m_osc->stop(); }, Qt::BlockingQueuedConnection);
        m_oscThread->quit();
        m_oscThread->wait();
    }
    m_statusTimer.stop();
    m_renderTimer.stop();
    m_autosaveTimer.stop();
    for (auto &[id, o] : m_outputs) delete o.window;
}

// ---------------------------------------------------------------------------
// Show-control shortcuts
// ---------------------------------------------------------------------------

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    // Shift+1 / Shift+2 switch the left tabs (Media Bin / Layers), whatever the keyboard: the digit with Shift
    // (AZERTY) or the symbol it gives (QWERTY "!" "@", UK "\""). Not while typing in a text field.
    if (e->type() == QEvent::KeyPress && isActiveWindow()) {
        auto *k = static_cast<QKeyEvent *>(e);
        QWidget *f = QApplication::focusWidget();
        const bool typing = qobject_cast<QLineEdit *>(f) || qobject_cast<QAbstractSpinBox *>(f) ||
                            (f && f->inherits("QTextEdit")) || (f && f->inherits("QPlainTextEdit"));
        if (!typing && (k->modifiers() & Qt::ShiftModifier) && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            int tab = -1;
            if (k->key() == Qt::Key_1 || k->key() == Qt::Key_Exclam) tab = 0;
            if (k->key() == Qt::Key_2 || k->key() == Qt::Key_At || k->key() == Qt::Key_QuoteDbl) tab = 1;
            if (tab >= 0 && !k->isAutoRepeat()) {
                m_leftTabs->setCurrentIndex(tab);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(o, e);
}

bool MainWindow::handleControlKey(int key, Qt::KeyboardModifiers mods)
{
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    if (ctrl && key == Qt::Key_B) setBlackout(!m_composition->isBlackout());
    else if (ctrl && shift && key == Qt::Key_F) toggleAllOutputs(1);
    else if (ctrl && key == Qt::Key_F) toggleAllOutputs(2);
    else if (!ctrl && shift && key == Qt::Key_Space) sequenceBack();
    else if (!ctrl && key == Qt::Key_Space) sequenceGo();
    else return false;
    return true;
}

void MainWindow::setBlackout(bool on)
{
    m_composition->setBlackout(on);
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
    edit->addSeparator();
    // On macOS, Qt moves it to the application menu (Fulskrin ▸ Settings…, ⌘,)
    QAction *prefs = edit->addAction(QStringLiteral("Preferences…"), QKeySequence::Preferences, this,
                                     [this] { m_tabs->setCurrentWidget(m_tabs->widget(2)); }); // the Settings tab
    prefs->setMenuRole(QAction::PreferencesRole);
    if (prefs->shortcut().isEmpty()) prefs->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma)); // Linux / Windows

    QMenu *layer = menuBar()->addMenu(QStringLiteral("&Layer"));
    // A layer is created empty; media is then dropped onto it (Media Bin, Finder)
    layer->addAction(QStringLiteral("New Layer"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this,
                     &MainWindow::addEmptyLayer);
    QAction *dup = layer->addAction(QStringLiteral("Duplicate"), this, &MainWindow::duplicateCurrentLayer);
    dup->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    layer->addAction(QStringLiteral("Delete (Del in list)"), this, &MainWindow::removeCurrentLayer);
    layer->addSeparator();
    layer->addAction(QStringLiteral("New Group (selected layers go into it)"), QKeySequence(Qt::CTRL | Qt::Key_G), this,
                     &MainWindow::createGroup);
    layer->addAction(QStringLiteral("Ungroup"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), this,
                     &MainWindow::ungroupCurrent);
    layer->addAction(QStringLiteral("Lock / Unlock"), QKeySequence(Qt::CTRL | Qt::Key_L), this, &MainWindow::toggleLockCurrent);
    layer->addAction(QStringLiteral("Rename (F2 in list, or double-click)"), this,
                     [this] { m_layerTable->startRename(currentLayer()); });
    layer->addSeparator();
    layer->addAction(QStringLiteral("Move Up"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                     [this] { moveCurrentLayer(-1); });
    layer->addAction(QStringLiteral("Move Down"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                     [this] { moveCurrentLayer(+1); });
    layer->addSeparator();
    layer->addAction(QStringLiteral("Play / Pause"), this, &MainWindow::togglePlayCurrent);

    QMenu *seq = menuBar()->addMenu(QStringLiteral("&Sequence"));
    seq->addAction(QStringLiteral("GO — next step (Space)"), this, &MainWindow::sequenceGo);
    seq->addAction(QStringLiteral("GO BACK — previous step (Shift+Space)"), this, &MainWindow::sequenceBack);
    seq->addAction(QStringLiteral("Stop the waits (pre-waits and follows still to come)"), m_engine, &Engine::sequenceStop);
    seq->addSeparator();
    seq->addAction(QStringLiteral("Sequences…"), this, &MainWindow::openSequences);
    seq->addAction(QStringLiteral("Timelines…"), this, &MainWindow::openTimelines);

    QMenu *comp = menuBar()->addMenu(QStringLiteral("C&omposition"));
    comp->addAction(QStringLiteral("Composition Size (Composition tab)"), this, [this] { m_tabs->setCurrentWidget(m_tabs->widget(1)); });
    comp->addAction(QStringLiteral("New Viewport"), this, &MainWindow::addViewport);
    comp->addSeparator();
    QAction *outlines = comp->addAction(QStringLiteral("Show Outlines of Other Layers"));
    outlines->setCheckable(true);
    outlines->setChecked(true);
    connect(outlines, &QAction::toggled, m_view, &MappingView::setShowAllOutlines);

    QMenu *out = menuBar()->addMenu(QStringLiteral("&Output"));
    // Every viewport at once; each one also has its own buttons, in its Output tab
    m_fullscreenAction = out->addAction(QStringLiteral("All Viewports Fullscreen"));
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F));
    m_fullscreenAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_fullscreenAction, &QAction::triggered, this, [this] { toggleAllOutputs(2); });
    m_windowedAction = out->addAction(QStringLiteral("All Viewports in Windows"));
    m_windowedAction->setCheckable(true);
    m_windowedAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    m_windowedAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_windowedAction, &QAction::triggered, this, [this] { toggleAllOutputs(1); });
    out->addAction(QStringLiteral("Hide All Viewports"), this, [this] {
        m_modesBefore.clear();
        setAllOutputs({}, 0);
    });
    out->addSeparator();
    m_blackoutAction = out->addAction(QStringLiteral("Blackout (Fade)"));
    m_blackoutAction->setCheckable(true);
    m_blackoutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    m_blackoutAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_blackoutAction, &QAction::toggled, this, &MainWindow::setBlackout);

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

    // macOS: Qt gives application-menu roles from the action texts (an action starting with "Settings" became the
    // preferences item). Only the actions meant for that menu keep a role.
    for (QAction *menuAction : menuBar()->actions())
        if (QMenu *m = menuAction->menu())
            for (QAction *a : m->actions())
                if (a->menuRole() == QAction::TextHeuristicRole) a->setMenuRole(QAction::NoRole);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void MainWindow::syncOutputs()
{
    struct Want {
        quint64 id;
        QString name, screen;
        int mode;
    };
    std::vector<Want> want;
    {
        Engine::Lock lk(&m_engine->mutex());
        for (int i : m_engine->viewports())
            if (const Layer *l = m_engine->layer(i)) want.push_back({l->id, l->name, l->vpScreen, l->vpMode});
    }
    // Viewports gone: their window goes (once off screen)
    for (auto it = m_outputs.begin(); it != m_outputs.end();) {
        const quint64 id = it->first;
        if (std::none_of(want.begin(), want.end(), [id](const Want &w) { return w.id == id; })) {
            it->second.window->dispose();
            it = m_outputs.erase(it);
        } else {
            ++it;
        }
    }
    bool changed = false, front = false;
    for (const Want &w : want) {
        Output &o = m_outputs[w.id];
        if (!o.window) {
            o.window = new OutputWindow(m_engine, w.id);
            const quint64 id = w.id;
            connect(o.window, &OutputWindow::closeRequested, this, [this, id] {
                const int i = m_engine->indexOfId(id);
                Engine::Lock lk(&m_engine->mutex());
                if (const Layer *l = m_engine->layer(i)) m_engine->setViewportOutput(i, l->vpScreen, 0);
            });
            // With the output window in front (a click on the projector, or fullscreen on the main screen),
            // the show-control shortcuts still work
            connect(o.window, &OutputWindow::keyPressed, this,
                    [this](int key, Qt::KeyboardModifiers mods) { handleControlKey(key, mods); });
        }
        const QString title = QStringLiteral("Fulskrin — %1").arg(w.name);
        if (o.title != title) o.window->setTitle(o.title = title);
        QScreen *sc = OutputWindow::screenNamed(w.screen);
        const QString scName = sc ? sc->name() : QString();
        if (o.mode == w.mode && (w.mode == 0 || o.screen == scName)) continue;
        o.mode = w.mode;
        o.screen = scName;
        changed = true;
        m_autosaveDone = false; // part of the session to restore
        if (w.mode == 0) {
            o.window->hideOutput();
            continue;
        }
        o.window->showOn(sc, w.mode == 2);
        if (w.mode == 2 && sc == screen()) {
            // Fullscreen on the screen of the interface: the output comes to the front and keeps the keyboard
            o.window->raise();
            o.window->requestActivate();
            front = true;
        }
    }
    const auto all = [&](int mode) {
        return !want.empty() && std::all_of(want.begin(), want.end(), [mode](const Want &w) { return w.mode == mode; });
    };
    m_fullscreenAction->setChecked(all(2));
    m_windowedAction->setChecked(all(1));
    if (front) statusBar()->showMessage(QStringLiteral("Fullscreen on the main screen: ⌘F / Ctrl+F to return."), 8000);
    else if (changed) activateWindow();
}

void MainWindow::setAllOutputs(const std::map<quint64, int> &modes, int otherwise)
{
    for (int i : m_engine->viewports()) {
        QString screen;
        quint64 id = 0;
        {
            Engine::Lock lk(&m_engine->mutex());
            const Layer *l = m_engine->layer(i);
            if (!l) continue;
            screen = l->vpScreen;
            id = l->id;
        }
        const auto it = modes.find(id);
        m_engine->setViewportOutput(i, screen, it != modes.end() ? it->second : otherwise);
    }
    m_inspector->refreshDynamic(); // the Output tab of the selected viewport
}

// First time: every viewport goes to `mode`, each on its own screen. Second time: back to how each one was
// (hidden if they already all were in that mode).
void MainWindow::toggleAllOutputs(int mode)
{
    std::map<quint64, int> now;
    {
        Engine::Lock lk(&m_engine->mutex());
        for (int i : m_engine->viewports())
            if (const Layer *l = m_engine->layer(i)) now[l->id] = l->vpMode;
    }
    const bool allThere = !now.empty() && std::all_of(now.begin(), now.end(), [mode](const auto &p) { return p.second == mode; });
    if (!allThere) {
        m_modesBefore = now;
        setAllOutputs({}, mode);
        return;
    }
    std::map<quint64, int> back = m_modesBefore;
    m_modesBefore.clear();
    if (back == now) back.clear(); // nothing to go back to: hidden
    setAllOutputs(back, 0);
}

void MainWindow::addViewport()
{
    const int i = m_engine->addViewport();
    if (i < 0) return;
    m_undo->push(new cmd::AddLayer(m_engine, i, QStringLiteral("New Viewport")));
    selectLayer(i);
    statusBar()->showMessage(QStringLiteral("Viewport added: place it in the composition (Spatial), "
                                            "choose its screen (Output)."),
                             5000);
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
    case SourceType::Text: return QStringLiteral("T");
    case SourceType::Audio: return QStringLiteral("♪");
    case SourceType::Layer: return QStringLiteral("⧉");
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
            r.tag = l->isViewport ? QStringLiteral("▭")
                    : l->isGroup  ? QStringLiteral("▤")
                                  : layerTag(l->type != SourceType::None ? l->type : l->missingType);
            r.viewport = l->isViewport;
            r.group = l->isGroup;
            r.collapsed = l->isGroup && l->collapsed;
            r.locked = l->locked;
            r.lockedByGroup = !l->locked && m_engine->isLocked(i);
            r.effectsOn = l->effectsEnabled;
            // Depth, and hidden inside a folded group (at any level above)
            for (quint64 up = l->parent; up;) {
                const Layer *g = m_engine->layer(m_engine->indexOfId(up));
                if (!g) break;
                ++r.depth;
                r.hidden = r.hidden || g->collapsed;
                up = g->parent;
            }
            r.visible = l->visible;
            r.opacity = l->opacity;
            r.blend = blendModeName(l->blend);
            r.error = !l->error.isEmpty();
            r.noPicture = l->type == SourceType::Audio || (l->type == SourceType::None && l->missingType == SourceType::Audio);
            if (l->isViewport) {
                static const QString kShown[] = {QStringLiteral("hidden"), QStringLiteral("window"), QStringLiteral("fullscreen")};
                const QSize vs = l->viewportSize();
                r.source = QStringLiteral("viewport %1 × %2").arg(vs.width()).arg(vs.height());
                r.playback = QStringLiteral("%1 × %2 · %3").arg(vs.width()).arg(vs.height()).arg(kShown[std::clamp(l->vpMode, 0, 2)]);
                rows.push_back(r);
                continue;
            }
            switch (l->isGroup ? SourceType::Image : l->type) {
            case SourceType::Video:
            case SourceType::Audio:
            case SourceType::Image:
                r.source = l->isGroup ? QStringLiteral("group of %1 layer(s)").arg(m_engine->groupMembers(i).size())
                                      : QFileInfo(l->sourcePath).fileName();
                break;
            case SourceType::Isf: r.source = QStringLiteral("generator ") + QFileInfo(l->sourcePath).completeBaseName(); break;
            case SourceType::Text: r.source = QStringLiteral("text ") + l->text.content.left(40).replace('\n', ' '); break;
            case SourceType::Layer: {
                const Layer *src = m_engine->layer(m_engine->indexOfId(l->sourceLayer));
                r.source = QStringLiteral("layer %1 · %2")
                               .arg(src ? src->name : QStringLiteral("(gone)"),
                                    l->sourceTap == LayerTap::PreFx ? QStringLiteral("pre-fx") : QStringLiteral("post-fx"));
                r.error = r.error || !src;
                break;
            }
            default: r.source = l->missingType != SourceType::None ? QFileInfo(l->sourcePath).fileName() + QStringLiteral(" — missing")
                                                                    : QStringLiteral("(empty)");
            }
            QStringList fx;
            for (const auto &e : l->effects) fx << (e->enabled ? e->name() : QStringLiteral("(") + e->name() + QStringLiteral(")"));
            r.effects = fx.join(QStringLiteral(" › "));
            r.effectCount = int(l->effects.size());
            if (l->audio) {
                const QString vol = l->muted ? QStringLiteral("muted") : QStringLiteral("%1%").arg(std::lround(l->volume * 100));
                r.source += (l->type == SourceType::Video ? QStringLiteral("   ♪ ") : QStringLiteral("   ")) + vol;
            }
            if (l->type == SourceType::Video || l->type == SourceType::Audio) {
                static const QString kModeSymbol[] = {QStringLiteral("→|"), QStringLiteral("↻"), QStringLiteral("⇄"),
                                                      QStringLiteral("→■")};
                r.playback = QStringLiteral("%1 %2 / %3  %4")
                                 .arg(l->ended ? QStringLiteral("■") : !l->playing ? QStringLiteral("❚❚") : l->speed < 0 ? QStringLiteral("◀") : QStringLiteral("▶"),
                                      fmtClock(l->position()), fmtClock(l->duration()), kModeSymbol[int(l->mode)]);
            } else if (l->type == SourceType::Isf || l->type == SourceType::Text) {
                r.playback = QStringLiteral("real time");
            }
            rows.push_back(r);
        }
    }
    const int n = int(rows.size());
    const int keep = qBound(-1, m_layerTable->currentRow(), n - 1);
    m_layerTable->setRows(rows);
    // Nothing selected yet: the first layer below the viewports (or the first viewport)
    int firstLayer = 0;
    while (firstLayer < n && rows[size_t(firstLayer)].viewport) ++firstLayer;
    const int sel = keep >= 0 ? keep : n == 0 ? -1 : firstLayer < n ? firstLayer : 0;
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
    if (refuseLocked(i)) return false;
    if (isTextGeneratorPath(path)) { // the built-in Text generator, dragged from the Media Bin
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(i);
            if (l && (l->isGroup || l->isViewport)) {
                lk.unlock();
                statusBar()->showMessage(QStringLiteral("A group or a viewport has no source: drop the text onto a layer."), 6000);
                return false;
            }
        }
        const QJsonObject before = m_engine->layerJson(i);
        if (!m_engine->setLayerText(i)) return false;
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(i);
            if (l && l->name.startsWith(QStringLiteral("Layer "))) l->name = QStringLiteral("Text");
        }
        m_undo->push(new cmd::ReplaceLayer(m_engine, i, before, QStringLiteral("Load Text Generator")));
        selectLayer(i);
        refreshAll();
        return true;
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
        if (h.isFilter) { // an ISF effect joins the layer's (or the group's) effect chain
            const QJsonArray before = m_engine->effectsJson(i);
            m_engine->addEffect(i, path);
            m_undo->push(new cmd::SetEffects(m_engine, i, before, QStringLiteral("Add Effect %1").arg(fi.completeBaseName())));
            selectLayer(i);
            refreshAll();
            return true;
        }
    }
    {
        Engine::Lock lk(&m_engine->mutex());
        if (m_engine->layer(i)->isGroup) {
            lk.unlock();
            statusBar()->showMessage(QStringLiteral("A group has no source: drop the media onto one of its layers."), 6000);
            return false;
        }
        if (m_engine->layer(i)->isViewport) {
            lk.unlock();
            statusBar()->showMessage(QStringLiteral("A viewport has no source: it shows the composition. "
                                                    "Drop the media onto a layer."), 6000);
            return false;
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
        if (isTextGeneratorPath(p)) { // built in: never imported into the Media Bin
            if (!sourceLoaded) sourceLoaded = loadIntoLayer(layer, p);
        } else if (effect) {
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

bool MainWindow::refuseLocked(int layer)
{
    if (!m_engine->isLocked(layer)) return false;
    statusBar()->showMessage(QStringLiteral("This layer is locked: unlock it (padlock) to edit it."), 4000);
    return true;
}

// A new layer (above the selected one) with that media: one undo step
void MainWindow::loadIntoNewLayer(const QString &path)
{
    m_undo->beginMacro(QStringLiteral("Load into New Layer"));
    addEmptyLayer();
    const bool ok = loadIntoLayer(currentLayer(), path);
    m_undo->endMacro();
    if (!ok) m_undo->undo(); // nothing loaded: no empty layer left behind
}

void MainWindow::addEmptyLayer()
{
    // Above the selected layer; inside its group if it is a member (or an open group's first layer)
    int at = 0;
    const int cur = currentLayer();
    if (cur >= 0 && m_engine->groupIndexOf(cur) >= 0) at = cur;
    if (cur >= 0 && m_engine->groupIndexOf(cur) >= 0 && m_engine->isLocked(m_engine->groupIndexOf(cur))) at = 0;
    int i = m_engine->addLayer(QString(), at);
    m_undo->push(new cmd::AddLayer(m_engine, i, QStringLiteral("Add Layer")));
    selectLayer(i);
}

void MainWindow::removeCurrentLayer()
{
    QList<int> rows = m_layerTable->selectedRows();
    if (rows.isEmpty() && currentLayer() >= 0) rows << currentLayer();
    if (rows.isEmpty()) return;
    // A group goes with its layers; locked layers stay
    QSet<int> victims;
    int skipped = 0;
    for (int r : rows) {
        if (m_engine->isLocked(r)) {
            ++skipped;
            continue;
        }
        const QList<int> members = m_engine->groupMembers(r);
        bool lockedMember = false;
        for (int m : members) lockedMember |= m_engine->isLocked(m);
        if (lockedMember) {
            ++skipped;
            continue;
        }
        victims.insert(r);
        for (int m : members) victims.insert(m);
    }
    if (skipped) statusBar()->showMessage(QStringLiteral("%1 locked layer(s) not deleted.").arg(skipped), 4000);
    if (victims.isEmpty()) return;
    QList<int> order = victims.values();
    std::sort(order.begin(), order.end(), std::greater<int>()); // members (below their group) first
    const int first = order.last();
    m_undo->beginMacro(order.size() > 1 ? QStringLiteral("Delete %1 Layers").arg(order.size()) : QStringLiteral("Delete Layer"));
    for (int r : order) m_undo->push(new cmd::RemoveLayer(m_engine, r));
    m_undo->endMacro();
    selectLayer(qMin(first, m_engine->layerCount() - 1));
}

void MainWindow::duplicateCurrentLayer()
{
    const int i = currentLayer();
    if (i < 0) return;
    const int members = m_engine->groupMembers(i).size();
    const int ni = m_engine->duplicateLayer(i);
    if (ni < 0) return;
    m_undo->beginMacro(members ? QStringLiteral("Duplicate Group") : QStringLiteral("Duplicate Layer"));
    for (int k = 0; k <= members; ++k) m_undo->push(new cmd::AddLayer(m_engine, ni + k, QStringLiteral("Duplicate Layer")));
    m_undo->endMacro();
    selectLayer(ni);
}

// ---------------------------------------------------------------------------
// Copy / paste of layer parameters (right-click on a layer)
// ---------------------------------------------------------------------------

void MainWindow::copyLayerParams(int row)
{
    const QJsonObject o = m_engine->layerJson(row);
    if (o.isEmpty()) return;
    m_paramClipboard = o;
    m_paramClipboardName = o.value("name").toString();
    statusBar()->showMessage(QStringLiteral("Parameters of \"%1\" copied.").arg(m_paramClipboardName), 4000);
}

// Pastes the chosen parts onto every selected layer (the one under the cursor if nothing is selected)
void MainWindow::pasteLayerParams(int parts, const QString &what)
{
    if (m_paramClipboard.isEmpty()) return;
    QList<int> rows = m_layerTable->selectedRows();
    if (rows.isEmpty() && currentLayer() >= 0) rows << currentLayer();
    QList<int> targets;
    for (int i : rows)
        if (i >= 0 && i < m_engine->layerCount() && !m_engine->isLocked(i)) targets << i;
    if (targets.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to paste onto (locked layer?)."), 4000);
        return;
    }
    const QString text = QStringLiteral("Paste %1").arg(what);
    if (targets.size() > 1) m_undo->beginMacro(text);
    for (int i : targets) {
        const QJsonObject before = m_engine->layerJson(i);
        if (m_engine->applyLayerParts(i, m_paramClipboard, parts))
            m_undo->push(new cmd::ReplaceLayer(m_engine, i, before, text));
    }
    if (targets.size() > 1) m_undo->endMacro();
    refreshAll();
    statusBar()->showMessage(QStringLiteral("%1 from \"%2\" onto %3 layer(s).").arg(text, m_paramClipboardName).arg(targets.size()), 4000);
}

void MainWindow::layerContextMenu(int row, const QPoint &globalPos)
{
    QMenu menu(this);
    const bool onLayer = row >= 0 && row < m_engine->layerCount();
    bool group = false, locked = false;
    if (onLayer) {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(row);
        group = l && (l->isGroup || l->isViewport); // no source of their own
        locked = m_engine->isLocked(row);
    }
    if (onLayer) {
        menu.addAction(QStringLiteral("Rename"), this, [this, row] { m_layerTable->startRename(row); })->setEnabled(!locked);
        menu.addAction(QStringLiteral("Duplicate"), this, &MainWindow::duplicateCurrentLayer);
        menu.addAction(locked ? QStringLiteral("Unlock") : QStringLiteral("Lock"), this, &MainWindow::toggleLockCurrent);
        menu.addAction(QStringLiteral("Delete"), this, &MainWindow::removeCurrentLayer)->setEnabled(!locked);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Copy Parameters"), this, [this, row] { copyLayerParams(row); });
    }
    QMenu *paste = menu.addMenu(m_paramClipboardName.isEmpty()
                                    ? QStringLiteral("Paste Parameters")
                                    : QStringLiteral("Paste Parameters from \"%1\"").arg(m_paramClipboardName));
    paste->setEnabled(onLayer && !locked && !m_paramClipboard.isEmpty());
    if (paste->isEnabled()) {
        auto add = [&](const QString &label, int parts, const QString &what) {
            paste->addAction(label, this, [this, parts, what] { pasteLayerParams(parts, what); });
        };
        add(QStringLiteral("Everything"), Engine::PartAll, QStringLiteral("Parameters"));
        paste->addSeparator();
        if (!group) add(QStringLiteral("Source"), Engine::PartSource, QStringLiteral("Source"));
        add(QStringLiteral("ROI"), Engine::PartRoi, QStringLiteral("ROI"));
        add(QStringLiteral("Color"), Engine::PartColor, QStringLiteral("Color"));
        add(QStringLiteral("Spatial"), Engine::PartSpatial, QStringLiteral("Spatial"));
        add(QStringLiteral("Effects"), Engine::PartEffects, QStringLiteral("Effects"));
        add(QStringLiteral("Compositing"), Engine::PartCompositing, QStringLiteral("Compositing"));
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("New Layer"), this, &MainWindow::addEmptyLayer);
    menu.exec(globalPos);
}

// Up / down among the layers of the same level (a group moves with its layers)
void MainWindow::moveCurrentLayer(int delta)
{
    const int i = currentLayer();
    if (i < 0 || refuseLocked(i)) return;
    const LayerTree t = m_engine->structure();
    const TreeNode cur = t[size_t(i)];
    std::vector<quint64> siblings;
    for (const TreeNode &n : t)
        if (n.parent == cur.parent) siblings.push_back(n.id);
    const int k = int(std::find(siblings.begin(), siblings.end(), cur.id) - siblings.begin());
    quint64 before = 0;
    if (delta < 0) {
        if (k == 0) return;
        before = siblings[size_t(k - 1)];
    } else {
        if (k + 1 >= int(siblings.size())) return;
        if (k + 2 < int(siblings.size())) {
            before = siblings[size_t(k + 2)];
        } else if (cur.parent) { // last layer of its group: before what follows the group
            const int g = tree::indexOf(t, cur.parent);
            for (size_t x = size_t(g) + 1; x < t.size(); ++x)
                if (t[x].parent != cur.parent) {
                    before = t[x].id;
                    break;
                }
        }
    }
    const LayerTree after = tree::moved(t, {cur.id}, before, cur.parent);
    if (after == t) return;
    m_undo->push(new cmd::SetStructure(m_engine, t, after, QStringLiteral("Reorder Layers")));
    selectLayer(m_engine->indexOfId(cur.id));
}

void MainWindow::moveRows(const QList<int> &rows, int beforeRow, int parentRow)
{
    const LayerTree t = m_engine->structure();
    std::vector<quint64> ids;
    int skipped = 0;
    for (int r : rows) {
        if (r < 0 || r >= int(t.size())) continue;
        if (m_engine->isLocked(r)) {
            ++skipped;
            continue;
        }
        ids.push_back(t[size_t(r)].id);
    }
    if (skipped) statusBar()->showMessage(QStringLiteral("Locked layers stay where they are."), 4000);
    if (ids.empty()) return;
    if (parentRow >= 0 && m_engine->isLocked(parentRow)) {
        statusBar()->showMessage(QStringLiteral("This group is locked."), 4000);
        return;
    }
    const quint64 before = beforeRow >= 0 && beforeRow < int(t.size()) ? t[size_t(beforeRow)].id : 0;
    const quint64 parent = parentRow >= 0 && parentRow < int(t.size()) ? t[size_t(parentRow)].id : 0;
    const LayerTree after = tree::moved(t, ids, before, parent);
    if (after == t) return;
    const bool into = parent && std::any_of(ids.begin(), ids.end(), [&](quint64 id) { return t[size_t(tree::indexOf(t, id))].parent != parent; });
    m_undo->push(new cmd::SetStructure(m_engine, t, after, into ? QStringLiteral("Move into Group") : QStringLiteral("Move Layers")));
    selectLayer(m_engine->indexOfId(ids.front()));
}

void MainWindow::createGroup()
{
    // Selected layers and groups (not locked) go into the new group, placed where the first of them was
    const LayerTree t = m_engine->structure();
    std::vector<quint64> ids;
    int at = -1;
    for (int r : m_layerTable->selectedRows()) {
        if (r < 0 || r >= int(t.size()) || t[size_t(r)].kind == TreeNode::Viewport || m_engine->isLocked(r)) continue;
        ids.push_back(t[size_t(r)].id);
        if (at < 0 || r < at) at = r; // the new group goes where the first of them is, in the same group
    }
    if (at < 0) at = 0;
    m_undo->beginMacro(QStringLiteral("New Group"));
    const int gi = m_engine->addGroup(QString(), at);
    m_undo->push(new cmd::AddLayer(m_engine, gi, QStringLiteral("New Group")));
    if (!ids.empty()) {
        const LayerTree before = m_engine->structure();
        const LayerTree after = tree::intoGroup(before, ids, m_engine->layerId(gi));
        m_undo->push(new cmd::SetStructure(m_engine, before, after, QStringLiteral("Move into Group")));
    }
    m_undo->endMacro();
    selectLayer(m_engine->indexOfId(m_engine->layerId(gi)));
    statusBar()->showMessage(ids.empty() ? QStringLiteral("Empty group created: drag layers onto it.")
                                         : QStringLiteral("Group created with %1 layer(s).").arg(ids.size()),
                             4000);
}

void MainWindow::ungroupCurrent()
{
    int g = currentLayer();
    if (g < 0) return;
    const LayerTree t = m_engine->structure();
    if (!t[size_t(g)].isGroup()) g = m_engine->groupIndexOf(g); // a layer: its group
    if (g < 0) return;
    if (!t[size_t(g)].isGroup() || refuseLocked(g)) return;
    const quint64 gid = t[size_t(g)].id;
    m_undo->beginMacro(QStringLiteral("Ungroup"));
    m_undo->push(new cmd::SetStructure(m_engine, t, tree::ungrouped(t, gid), QStringLiteral("Ungroup")));
    m_undo->push(new cmd::RemoveLayer(m_engine, m_engine->indexOfId(gid)));
    m_undo->endMacro();
    selectLayer(qMin(g, m_engine->layerCount() - 1));
}

void MainWindow::toggleLockCurrent()
{
    QList<int> rows = m_layerTable->selectedRows();
    if (rows.isEmpty() && currentLayer() >= 0) rows << currentLayer();
    if (rows.isEmpty()) return;
    const bool lock = !cmd::SetLayerProp::read(m_engine, rows.first(), cmd::SetLayerProp::Locked).toBool();
    m_undo->beginMacro(lock ? QStringLiteral("Lock") : QStringLiteral("Unlock"));
    for (int r : rows) {
        const QVariant before = cmd::SetLayerProp::read(m_engine, r, cmd::SetLayerProp::Locked);
        if (before.isValid() && before.toBool() != lock)
            m_undo->push(new cmd::SetLayerProp(m_engine, r, cmd::SetLayerProp::Locked, before, lock));
    }
    m_undo->endMacro();
    m_inspector->rebuild();
}

void MainWindow::startOsc()
{
    // Runs in its own thread: OSC keeps answering while the interface is busy
    if (!m_oscThread) {
        m_oscThread = new QThread(this);
        m_oscThread->setObjectName("OSC");
        m_osc = new OscServer(m_engine);
        m_osc->moveToThread(m_oscThread);
        connect(m_oscThread, &QThread::finished, m_osc, &QObject::deleteLater);
        connect(m_osc, &OscServer::edited, this, [this] {
            markDirty();
            refreshLayerList();
            m_composition->syncFromEngine();
            m_inspectorTimer.start(); // the inspector follows (not during a gesture: it is rebuilt a bit later)
        });
        m_oscThread->start();
    }
    const bool on = SettingsPanel::oscEnabled();
    const int udp = SettingsPanel::oscPort(), http = SettingsPanel::oscQueryPort();
    QString status;
    QMetaObject::invokeMethod(
        m_osc,
        [this, on, udp, http, &status] {
            if (on) m_osc->start(quint16(udp), quint16(http));
            else m_osc->stop();
            status = m_osc->status();
        },
        Qt::BlockingQueuedConnection);
    m_settings->setOscStatus(on ? status : QStringLiteral("OSC off"));
    if (on) statusBar()->showMessage(status, 6000);
}

void MainWindow::setInOutAtPosition(bool in)
{
    const int i = currentLayer();
    if (i >= 0 && refuseLocked(i)) return;
    double pos;
    QVariant before;
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(i);
        if (!l || !l->hasTransport()) return;
        pos = l->position();
        before = in ? l->inPoint : l->outPoint;
    }
    const auto prop = in ? cmd::SetLayerProp::InPoint : cmd::SetLayerProp::OutPoint;
    m_undo->push(new cmd::SetLayerProp(m_engine, i, prop, before, pos));
    m_inspector->rebuild();
    statusBar()->showMessage((in ? QStringLiteral("In point: %1 s") : QStringLiteral("Out point: %1 s")).arg(pos, 0, 'f', 2), 3000);
}

void MainWindow::sequenceGo() { m_engine->sequenceGo(); }

void MainWindow::sequenceBack() { m_engine->sequenceBack(); }

// The step's GO: its pre-wait, then its snapshot (recalled through the undo stack, see the recaller), then what follows
void MainWindow::sequenceGoTo(int step) { m_engine->sequenceGoTo(step); }

void MainWindow::openSequences()
{
    m_seqWindow->show();
    m_seqWindow->raise();
    m_seqWindow->activateWindow();
}

void MainWindow::openTimelines()
{
    m_timelineWindow->show();
    m_timelineWindow->raise();
    m_timelineWindow->activateWindow();
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
    m_composition->refreshStatus();
    refreshLayerList(); // playback positions (in-place update)
    const int pct = int(std::lround(m_engine->outputLevel() * 100));
    const QSize c = m_engine->compositionSize();
    // Each viewport: shown how (F fullscreen, W window), and what it publishes
    QStringList outs;
    int shown = 0;
    for (const auto &[id, o] : m_outputs) {
        QStringList pubs;
        for (int k = 0; k < kPublishKindCount; ++k)
            if (m_engine->publishState(id, PublishKind(k)).level == PublishState::Ok) pubs << publishKindName(PublishKind(k));
        if (o.mode) ++shown;
        if (!pubs.isEmpty()) outs << QStringLiteral("%1 → %2").arg(o.title.section(QStringLiteral(" — "), 1), pubs.join('+'));
    }
    m_status->setText(QStringLiteral("%1 × %2   ·   %3 fps   ·   composition %4%   ·   %5 of %6 viewport(s) shown%7")
                          .arg(c.width())
                          .arg(c.height())
                          .arg(m_engine->fps(), 0, 'f', 1)
                          .arg(pct)
                          .arg(shown)
                          .arg(m_outputs.size())
                          .arg(outs.isEmpty() ? QString() : QStringLiteral("   ·   ") + outs.join(QStringLiteral(", ")))
                      + (m_engine->blackout() ? QStringLiteral("   ·   BLACKOUT") : QString()));
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
    m_settings->setTransitions(m_engine->library().transitions(), m_engine->defaultTransition());
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
    o["selected_layer"] = currentLayer();
    return o;
}

bool MainWindow::maybeSave()
{
    if (!isDirty()) return true;
    QMessageBox box(QMessageBox::Question, QStringLiteral("Fulskrin"), QStringLiteral("Save changes to the project?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    // ⌘D / Ctrl+D discards, as everywhere else on macOS
    if (auto *discard = qobject_cast<QPushButton *>(box.button(QMessageBox::Discard)))
        discard->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    box.setDefaultButton(QMessageBox::Save);
    const int r = box.exec();
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::afterProjectLoaded(const QJsonObject &ui)
{
    m_undo->clear();
    m_autosaveDone = false;
    m_modesBefore.clear();
    m_composition->syncFromEngine();
    m_bin->refresh();
    selectLayer(qBound(-1, ui.value("selected_layer").toInt(0), m_engine->layerCount() - 1));
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
    ui["autosave_of"] = m_engine->projectPath();
    ui["autosave_time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    ui["autosave_dirty"] = isDirty();
    QString err;
    if (m_engine->saveProject(autosavePath(), ui, &err)) {
        m_autosaveIndex = m_undo->index();
        m_autosaveDone = true;
    } else {
        statusBar()->showMessage(QStringLiteral("Autosave failed: ") + err, 6000);
    }
}

bool MainWindow::offerRecovery()
{
    const QString path = autosavePath();
    if (!QFile::exists(path)) return false;
    QFile f(path);
    QJsonObject ui;
    if (f.open(QIODevice::ReadOnly)) ui = QJsonDocument::fromJson(f.readAll()).object().value("ui").toObject();
    f.close();
    const QString original = ui.value("autosave_of").toString();
    const QDateTime when = QDateTime::fromString(ui.value("autosave_time").toString(), Qt::ISODate);
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
        return false;
    }
    QJsonObject loadedUi;
    QString err;
    m_engine->loadProject(path, &loadedUi, &err);
    m_engine->setProjectPath(original); // "Save" writes to the original project
    m_forceDirty = ui.value("autosave_dirty").toBool(true);
    afterProjectLoaded(loadedUi);
    if (!err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Recovery"), err);
    const bool shown = std::any_of(m_outputs.begin(), m_outputs.end(), [](const auto &p) { return p.second.mode != 0; });
    if (shown) {
        // The outputs come back, but blacked out: the operator decides when to bring them back up.
        m_engine->setBlackout(true, 0.0);
        setBlackout(true);
        statusBar()->showMessage(QStringLiteral("Session restored — outputs blacked out: Ctrl+B (⌘B) to bring them back"), 15000);
    } else {
        statusBar()->showMessage(QStringLiteral("Session restored"), 6000);
    }
    return true;
}

// ---------------------------------------------------------------------------
// File opening (macOS: double-click .fulskrin file or drag to app icon)
// ---------------------------------------------------------------------------

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::FileOpen) {
        auto *fe = static_cast<QFileOpenEvent *>(e);
        const QString path = fe->file();
        if (!path.isEmpty()) {
            // If unsaved changes, ask before opening the new project
            if (!maybeSave()) return false;
            openProject(path);
            return true;
        }
    }
    return QMainWindow::event(e);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings s;
    s.setValue("ui/geometry", saveGeometry());
    if (auto *split = findChild<QSplitter *>("mainSplit2")) s.setValue("ui/mainSplit2", split->saveState());
    if (auto *top = findChild<QSplitter *>("topSplit2")) s.setValue("ui/topSplit2", top->saveState());
    for (auto &[id, o] : m_outputs) o.window->hideOutput();
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
