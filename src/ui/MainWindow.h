#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>

class Engine;
class MappingView;
class LayerInspector;
class OutputWindow;
class QListWidget;
class QLabel;
class QMenu;
class QAction;
class QActionGroup;
class QScreen;
class QSlider;
class QPushButton;
class QDoubleSpinBox;
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

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    QWidget *buildMasterPanel();
    void buildMenus();
    void buildOutputScreensMenu();
    void rebuildGeneratorMenus();
    void refreshLayerList();
    void refreshAll();
    void selectLayer(int index);
    int currentLayer() const;
    void statusTick();
    void updateTitle();

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

    int newLayerFromFile(const QString &path, int at = 0);
    void addVideoLayer();
    void addImageLayer();
    void setSourceFromDialog(const QString &kind);
    void addGeneratorLayer(const QString &path);
    void addEmptyLayer();
    void removeCurrentLayer();
    void duplicateCurrentLayer();
    void moveCurrentLayer(int delta);
    void togglePlayCurrent();

    void setOutputVisible(bool on);
    QScreen *selectedScreen() const;
    void compositionDialog();
    void addIsfFolder();
    void rescanLibrary();

    void setBlackout(bool on);

    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    MappingView *m_view = nullptr;
    LayerInspector *m_inspector = nullptr;
    OutputWindow *m_output = nullptr;
    QListWidget *m_layers = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_statusTimer, m_renderTimer, m_autosaveTimer;

    QSlider *m_masterSlider = nullptr;
    QPushButton *m_blackoutButton = nullptr;
    QDoubleSpinBox *m_fadeTime = nullptr;
    QLabel *m_masterLabel = nullptr;
    QAction *m_blackoutAction = nullptr;

    QMenu *m_generatorMenu = nullptr, *m_screensMenu = nullptr, *m_addMenu = nullptr;
    QActionGroup *m_screenGroup = nullptr;
    QAction *m_outputAction = nullptr, *m_undoAction = nullptr, *m_redoAction = nullptr;
    QString m_screenName;
    bool m_refreshingList = false;

    bool m_forceDirty = false;          // session restaurée non enregistrée
    bool m_autosaveEnabled = true;
    int m_autosaveIndex = -1;           // position de la pile d'annulation lors de la dernière sauvegarde auto
    bool m_autosaveDone = false;
};
