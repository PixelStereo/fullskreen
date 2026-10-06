#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>
#include <map>

class Engine;
class MappingView;
class LayerInspector;
class LayerTable;
class MediaBin;
class CompositionPanel;
class SettingsPanel;
class SnapshotPanel;
class SequenceBar;
class SequenceWindow;
class TimelineWindow;
class OutputWindow;
class QLabel;
class QMenu;
class QAction;
class QActionGroup;
class QScreen;
class QTabWidget;
class QUndoStack;
class OscServer;
class QThread;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Engine *engine, QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString &path);
    // Offers to restore the session if the application did not exit normally.
    bool offerRecovery(); // true: the autosaved session was restored
    bool maybeSave();     // unsaved changes: save, discard, or cancel (false)
    void setAutosaveEnabled(bool on) { m_autosaveEnabled = on; }
    void setQuiet(bool on) { m_quiet = on; } // tests: warnings in the status bar, no dialog

protected:
    bool event(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void buildMenus();
    void refreshLayerList();
    void refreshAll();
    void selectLayer(int index);
    int currentLayer() const;
    void statusTick();
    void updateTitle();
    void markDirty();

    void newProject();
    void openProjectDialog();
    bool save();
    bool saveAs();
    bool saveTo(const QString &path);
    bool isDirty() const;
    QJsonObject uiState() const;
    void autosave();
    static QString autosavePath();
    void afterProjectLoaded(const QJsonObject &ui);

    bool loadIntoLayer(int layer, const QString &path); // media, ISF generator or ISF effect (undoable)
    void loadDropped(int layer, const QStringList &paths);
    void addEmptyLayer();
    void removeCurrentLayer(); // the selected layers (a group with its layers)
    void createGroup();         // the selected layers go into it
    void ungroupCurrent();
    void toggleLockCurrent();
    void moveRows(const QList<int> &rows, int beforeRow, int parentRow);
    void startOsc();
    bool refuseLocked(int layer); // status message if locked
    void duplicateCurrentLayer();
    void moveCurrentLayer(int delta);
    void layerContextMenu(int row, const QPoint &globalPos);
    void copyLayerParams(int row);                             // into m_paramClipboard
    void pasteLayerParams(int parts, const QString &what);     // Engine::LayerParts, onto the selected layers
    void togglePlayCurrent();
    // Sequences: GO plays the next step of the current sequence, GO BACK the previous one (Space, Shift+Space)
    void sequenceGo();
    void sequenceBack();
    void sequenceGoTo(int step);
    void openSequences();
    void setInOutAtPosition(bool in);
    void relinkMedia(const QString &from, const QString &to);

    // Outputs: one window per viewport, shown on its screen as its Output tab says
    void syncOutputs();
    void toggleAllOutputs(int mode); // ⌘F (fullscreen) / ⌘⇧F (window): every viewport, then back to how each was
    void setAllOutputs(const std::map<quint64, int> &modes, int otherwise);
    void addViewport();
    void addIsfFolder();
    void rescanLibrary();

    void setBlackout(bool on);
    bool handleControlKey(int key, Qt::KeyboardModifiers mods); // show-control shortcuts, also from the output window

    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    MappingView *m_view = nullptr;
    LayerInspector *m_inspector = nullptr;
    LayerTable *m_layerTable = nullptr;
    MediaBin *m_bin = nullptr;
    CompositionPanel *m_composition = nullptr;
    SettingsPanel *m_settings = nullptr;
    SnapshotPanel *m_snapshots = nullptr;
    SequenceBar *m_seqBar = nullptr;
    SequenceWindow *m_seqWindow = nullptr;
    TimelineWindow *m_timelineWindow = nullptr;
    void openTimelines();
    int m_cueUndoIndex = -1; // undo index when the current step was played: anything since is a change
    QTabWidget *m_leftTabs = nullptr;
    QTabWidget *m_tabs = nullptr;
    struct Output {
        OutputWindow *window = nullptr;
        QString screen, title;
        int mode = 0;
    };
    std::map<quint64, Output> m_outputs;      // by viewport id
    std::map<quint64, int> m_modesBefore;     // modes before ⌘F / ⌘⇧F, to go back to
    QLabel *m_status = nullptr;
    OscServer *m_osc = nullptr;
    QThread *m_oscThread = nullptr;
    QTimer m_statusTimer, m_renderTimer, m_autosaveTimer, m_binTimer, m_inspectorTimer;

    QAction *m_fullscreenAction = nullptr, *m_windowedAction = nullptr, *m_undoAction = nullptr, *m_redoAction = nullptr,
            *m_blackoutAction = nullptr;
    int m_lastSelected = -1;
    QJsonObject m_paramClipboard;       // layer parameters copied with the right-click menu
    QString m_paramClipboardName;       // the layer they come from (shown in the menu)

    bool m_forceDirty = false;          // change outside the undo stack (composition, publishing, media bin)
    bool m_autosaveEnabled = true;
    bool m_quiet = false;
    int m_autosaveIndex = -1;           // undo stack index at the last autosave
    bool m_autosaveDone = false;
};
