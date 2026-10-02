#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>

class Engine;
class MappingView;
class LayerInspector;
class LayerTable;
class MediaBin;
class MasterPanel;
class OutputWindow;
class QLabel;
class QMenu;
class QAction;
class QActionGroup;
class QScreen;
class QTabWidget;
class QUndoStack;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Engine *engine, QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString &path);
    // Propose de restaurer la session si l'application ne s'est pas fermée normalement.
    void offerRecovery();
    void setAutosaveEnabled(bool on) { m_autosaveEnabled = on; }
    void setQuiet(bool on) { m_quiet = on; } // tests : avertissements dans la barre d'état, sans dialogue

    enum OutputMode { OutputHidden = 0, OutputWindowed = 1, OutputFullscreen = 2 };

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void buildMenus();
    void buildOutputScreensMenu();
    void rebuildGeneratorMenus();
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
    bool maybeSave();
    bool isDirty() const;
    QJsonObject uiState() const;
    void autosave();
    static QString autosavePath();
    void afterProjectLoaded(const QJsonObject &ui);

    int newLayerFromFile(const QString &path, int at = 0);
    void addVideoLayer();
    void addImageLayer();
    void setSourceFromDialog(const QString &kind);
    void setSourceFromFile(int layer, const QString &path);
    void addGeneratorLayer(const QString &path);
    void addEmptyLayer();
    void removeCurrentLayer();
    void duplicateCurrentLayer();
    void moveCurrentLayer(int delta);
    void togglePlayCurrent();
    void relinkMedia(const QString &from, const QString &to);

    void setOutputMode(OutputMode mode);
    void toggleFullscreen();
    void toggleWindowed();
    QScreen *selectedScreen() const;
    void chooseScreen(const QString &name);
    void addIsfFolder();
    void rescanLibrary();

    void setBlackout(bool on);
    bool handleControlKey(int key, Qt::KeyboardModifiers mods); // raccourcis de régie, aussi depuis la sortie

    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    MappingView *m_view = nullptr;
    LayerInspector *m_inspector = nullptr;
    LayerTable *m_layerTable = nullptr;
    MediaBin *m_bin = nullptr;
    MasterPanel *m_master = nullptr;
    QTabWidget *m_tabs = nullptr;
    OutputWindow *m_output = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_statusTimer, m_renderTimer, m_autosaveTimer, m_binTimer, m_inspectorTimer;

    QMenu *m_generatorMenu = nullptr, *m_screensMenu = nullptr;
    QActionGroup *m_screenGroup = nullptr;
    QAction *m_fullscreenAction = nullptr, *m_windowedAction = nullptr, *m_undoAction = nullptr, *m_redoAction = nullptr,
            *m_blackoutAction = nullptr;
    QString m_screenName;
    OutputMode m_outputMode = OutputHidden;
    int m_lastSelected = -1;

    bool m_forceDirty = false;          // modification hors pile d'annulation (composition, publication, chutier)
    bool m_autosaveEnabled = true;
    bool m_quiet = false;
    int m_autosaveIndex = -1;           // position de la pile d'annulation lors de la dernière sauvegarde auto
    bool m_autosaveDone = false;
};
