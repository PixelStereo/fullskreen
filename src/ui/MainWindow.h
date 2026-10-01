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

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Engine *engine, QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString &path);

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void buildMenus();
    void buildOutputScreensMenu();
    void rebuildGeneratorMenus();
    void refreshLayerList();
    void selectLayer(int index);
    int currentLayer() const;
    void tick();
    void updateTitle();

    void newProject();
    void openProjectDialog();
    bool save();
    bool saveAs();
    bool maybeSave();
    QJsonObject uiState() const;

    int newLayerFromFile(const QString &path, int at = 0);
    void addVideoLayer();
    void addImageLayer();
    void setSourceFromDialog(const QString &kind);
    void addGeneratorLayer(const QString &path);
    void removeCurrentLayer();
    void duplicateCurrentLayer();
    void moveCurrentLayer(int delta);
    void togglePlayCurrent();

    void setOutputVisible(bool on);
    QScreen *selectedScreen() const;
    void compositionDialog();
    void addIsfFolder();
    void rescanLibrary();

    Engine *m_engine;
    MappingView *m_view = nullptr;
    LayerInspector *m_inspector = nullptr;
    OutputWindow *m_output = nullptr;
    QListWidget *m_layers = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_timer;
    int m_tickCount = 0;

    QMenu *m_generatorMenu = nullptr, *m_screensMenu = nullptr, *m_addMenu = nullptr;
    QActionGroup *m_screenGroup = nullptr;
    QAction *m_outputAction = nullptr;
    QString m_screenName;
    bool m_refreshingList = false;
};
