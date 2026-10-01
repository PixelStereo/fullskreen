#pragma once
// Moteur de rendu : ne dépend que de QtCore/QtGui/OpenGL, aucune dépendance aux widgets.
// Il possède son propre contexte OpenGL hors écran, partagé avec les fenêtres d'affichage
// (prévisualisation, sortie) qui se contentent d'afficher la texture de sortie.

#include "IsfLibrary.h"
#include "Layer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QSize>

class QOpenGLContext;
class QOffscreenSurface;

class Engine : public QObject
{
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    bool initialize(QString *err);
    void shutdown();

    // Composition
    QSize compositionSize() const { return m_compSize; }
    void setCompositionSize(QSize s);

    int layerCount() const { return int(m_layers.size()); }
    Layer *layer(int i) { return (i >= 0 && i < layerCount()) ? m_layers[size_t(i)].get() : nullptr; }
    int indexOf(const Layer *l) const;

    int addLayer(const QString &name = {}, int at = 0); // index 0 = calque du dessus
    void removeLayer(int i);
    void moveLayer(int from, int to);
    int duplicateLayer(int i);

    bool setLayerVideo(int i, const QString &path, QString *err = nullptr);
    bool setLayerImage(int i, const QString &path, QString *err = nullptr);
    bool setLayerIsf(int i, const QString &path, QString *err = nullptr);
    void clearLayerSource(int i);
    void setGeneratorSize(int i, int w, int h);

    void setLayerPlaying(int i, bool playing);
    void setLayerLoop(int i, bool loop);
    void seekLayer(int i, double t);

    int addEffect(int layerIndex, const QString &path, QString *err = nullptr);
    void removeEffect(int layerIndex, int fx);
    void moveEffect(int layerIndex, int from, int to);
    bool setIsfImageInput(IsfInstance *inst, int input, const QString &path, QString *err = nullptr);
    bool reloadIsf(IsfInstance *inst); // recharge depuis le disque (édition en direct)

    // Rendu d'une frame complète (appelé par l'horloge de l'UI).
    void renderFrame();
    GLuint outputTexture() const { return m_output.tex; }
    double fps() const { return m_fps; }
    QImage grabOutput(); // lecture GPU -> image (tests, captures)

    // Projet
    void newProject();
    bool saveProject(const QString &path, const QJsonObject &uiState, QString *err);
    bool loadProject(const QString &path, QJsonObject *uiState, QString *err);
    QString projectPath() const { return m_projectPath; }

    IsfLibrary &library() { return m_library; }

signals:
    void layersChanged();
    void compositionSizeChanged(QSize size);

private:
    friend struct ScopedCurrent;
    void makeCurrent();
    void doneCurrent();
    void releaseLayer(Layer &l);
    void updateSource(Layer &l, double dt);
    void renderLayer(Layer &l, const IsfRenderContext &rc);
    void composite();
    void drawQuad();
    void blit(GLuint tex, const RenderTarget &target);
    QString resolvePath(const QJsonObject &o, const QString &projectDir) const;
    QJsonObject layerToJson(const Layer &l, const QString &projectDir) const;
    void layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings);

    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;

    GLuint m_quadVao = 0, m_quadVbo = 0;
    GLuint m_meshVao = 0, m_meshVbo = 0, m_meshIbo = 0;
    GLsizei m_meshIndexCount = 0;
    GLuint m_blitProgram = 0, m_compProgram = 0;
    GLint m_blitTexLoc = -1, m_compTexLoc = -1, m_compOpacityLoc = -1;
    GLuint m_blackTex = 0;
    RenderTarget m_output;

    QSize m_compSize{1920, 1080};
    std::vector<std::unique_ptr<Layer>> m_layers;
    std::vector<float> m_meshScratch;

    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
    double m_fps = 0;
    bool m_initialized = false;

    QString m_projectPath;
    IsfLibrary m_library;
};
