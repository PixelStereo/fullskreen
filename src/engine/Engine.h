#pragma once
// Render engine: depends only on QtCore/QtGui/OpenGL, no dependency on widgets.
//
// Two modes:
//  - dedicated thread (start()): a QThread owns the OpenGL context, renders the composition and presents
//    it itself in the output window, locked to vertical sync. The UI may freeze
//    (dialog, loading) without the output stopping;
//  - manual (no start()): renderFrame() is called by the thread that owns the engine (tests, command-line rendering).
//
// Concurrency:
//  - composition data (layers, parameters, mapping, master) is protected by mutex();
//    the UI takes Engine::Lock before reading or writing a Layer directly;
//  - anything touching OpenGL (shader compilation, texture release…) goes through runGl(),
//    which runs it on the render thread. These tasks never take the lock: it is safe to wait
//    on a task while holding the lock, with no risk of deadlock.

#include "AudioOutput.h"
#include "IsfLibrary.h"
#include "Layer.h"
#include "LayerTree.h"
#include "Publish.h"

#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QRecursiveMutex>
#include <QSize>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

class QOpenGLContext;
class QOffscreenSurface;
class QSurface;
class QThread;
class QWindow;

class Engine : public QObject
{
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    using Lock = QMutexLocker<QRecursiveMutex>;
    QRecursiveMutex &mutex() { return m_mutex; }

    bool initialize(QString *err);
    bool start();          // starts the render thread (returns false if the platform does not support it)
    void stop();
    bool isThreaded() const { return m_threaded; }
    void shutdown();

    // Runs fn on the render thread with the OpenGL context current (or immediately in manual mode).
    void runGl(std::function<void()> fn, bool wait = true);

    // --- Composition (take Lock to access Layer objects)
    QSize compositionSize() const;
    void setCompositionSize(QSize s);

    int layerCount() const;
    Layer *layer(int i);
    int indexOf(const Layer *l) const;

    int addLayer(const QString &name = {}, int at = 0); // index 0 = top layer
    int addGroup(const QString &name = {}, int at = 0);  // empty group (members are moved in with setStructure)
    // Removes a layer; a removed group's members go back to the top level.
    void removeLayer(int i);
    void moveLayer(int from, int to);
    // Duplicates a layer before itself (a group with its members); returns the index of the copy (the group's).
    int duplicateLayer(int i);

    // --- Structure: order and groups (see LayerTree.h). Members immediately follow their group.
    quint64 layerId(int i) const;
    int indexOfId(quint64 id) const;
    LayerTree structure() const;
    void setStructure(const LayerTree &t); // reorders and regroups (no layer is created or destroyed)
    int groupIndexOf(int i) const;         // index of the group containing layer i, -1 at the top level
    QList<int> groupMembers(int group) const;
    bool isLocked(int i) const;            // locked itself, or member of a locked group

    // JSON snapshot of a layer, and re-creation (undo / redo, duplicate)
    QJsonObject layerJson(int i) const;
    int insertLayerJson(int at, const QJsonObject &o);
    void replaceLayerJson(int i, const QJsonObject &o);
    QJsonArray effectsJson(int i) const;
    void setEffectsJson(int i, const QJsonArray &a);

    bool setLayerVideo(int i, const QString &path, QString *err = nullptr);
    bool setLayerImage(int i, const QString &path, QString *err = nullptr);
    bool setLayerIsf(int i, const QString &path, QString *err = nullptr);
    bool setLayerAudio(int i, const QString &path, QString *err = nullptr);
    // Video, image or audio according to the file: a video file without a picture becomes an audio layer.
    bool setLayerFile(int i, const QString &path, QString *err = nullptr);
    void clearLayerSource(int i);
    void setGeneratorSize(int i, int w, int h);

    void setLayerPlaying(int i, bool playing);
    void setLayerPlayMode(int i, PlayMode mode);
    void setLayerSpeed(int i, double speed); // negative: backwards (changing direction keeps the position)
    // In / out points (seconds; out < 0: end of the media). Playback, loops and ping-pong stay within them.
    void setLayerInOut(int i, double in, double out);
    // Mode given to a video or a sound when it is loaded into a layer (preference)
    void setDefaultPlayMode(PlayMode m) { m_defaultPlayMode = m; }
    PlayMode defaultPlayMode() const { return m_defaultPlayMode; }
    void seekLayer(int i, double t);
    void setLayerVolume(int i, float volume);
    void setLayerMuted(int i, bool muted);

    int addEffect(int layerIndex, const QString &path, QString *err = nullptr);
    void removeEffect(int layerIndex, int fx);
    void moveEffect(int layerIndex, int from, int to);
    bool setIsfImageInput(IsfInstance *inst, int input, const QString &path, QString *err = nullptr);
    bool reloadIsf(IsfInstance *inst); // reloads from disk (live editing)

    // --- External media (media bin)
    struct MediaRef {
        QString path;
        bool video = false;      // video file
        bool audio = false;      // audio file (neither: image)
        bool missing = false;
        bool imported = false;   // added to the media bin by the user
        QStringList users;       // "Layer" or "Layer › Effect"
    };
    std::vector<MediaRef> mediaUsage() const;
    QStringList binItems() const;
    void addBinItems(const QStringList &paths);
    void removeBinItem(const QString &path);
    QList<int> layersUsingMedia(const QString &path) const;
    // Replaces file `from` with `to` in a layer (source and ISF image inputs). Returns true if changed.
    bool relinkLayerMedia(int layer, const QString &from, const QString &to, QString *err = nullptr);
    void relinkBinItem(const QString &from, const QString &to);
    static bool isVideoFile(const QString &path);
    static bool isImageFile(const QString &path);
    static bool isAudioFile(const QString &path);
    static QStringList videoExtensions();
    static QStringList imageExtensions();
    static QStringList audioExtensions();

    // --- Sound: one output device, mixing the audio of every layer
    bool startAudio(const QString &device, QString *err, bool nullDevice = false); // empty = system default
    void stopAudio();
    AudioOutput &audioOutput() { return *m_audio; }
    float audioVolume() const { return m_audio->masterVolume(); }
    void setAudioVolume(float v) { m_audio->setMasterVolume(v); }
    bool audioMuted() const { return m_audio->muted(); }
    void setAudioMuted(bool m) { m_audio->setMuted(m); }

    // --- Output publishing (NDI, OMT, Syphon, Spout)
    void setPublishSettings(const PublishSettings &s);
    PublishSettings publishSettings() const;
    PublishState publishState(PublishKind k) const;
    // Tests: receives the read-back frames (send thread)
    void setTestTap(std::function<void(const CpuFrame &)> fn);

    // --- Master fader: level 0..1 reached in `seconds` seconds (picture only)
    void fadeMaster(double target, double seconds);
    double masterLevel() const { return m_masterLevel.load(); }
    double masterTarget() const;
    // Blackout: fades the picture and the sound out (and back in) in `seconds` seconds
    void setBlackout(bool on, double seconds);
    void setBlackout(bool on) { setBlackout(on, blackoutFade()); }
    bool blackout() const;
    double blackoutLevel() const { return m_blackLevel.load(); }
    void setBlackoutFade(double seconds);  // default fade duration
    double blackoutFade() const;
    double outputLevel() const { return masterLevel() * blackoutLevel(); }

    // --- Source preview (crop editor): a downscaled copy of a layer's source picture (before crop),
    // read back by the render thread every few frames while requested.
    void requestSourcePreview(quint64 layerId, int maxSide = 360); // 0: stop
    QImage sourcePreview(quint64 *layerId = nullptr) const;

    // --- Output: window in which the render thread presents the composition
    void setOutputWindow(QWindow *w);
    void setOutputExposed(bool exposed, QSize pixelSize);

    // --- Rendering
    void renderFrame();                  // manual mode only
    GLuint outputTexture() const;        // last published frame (readable from a shared context)
    double fps() const { return m_fps.load(); }
    QImage grabOutput();
    quint64 frameCount() const { return m_frameCount.load(); }

    // --- Project
    void newProject();
    bool saveProject(const QString &path, const QJsonObject &uiState, QString *err);
    bool loadProject(const QString &path, QJsonObject *uiState, QString *err);
    QString projectPath() const;
    void setProjectPath(const QString &p);

    IsfLibrary &library() { return m_library; }

signals:
    void layersChanged();
    void compositionSizeChanged(QSize size);
    void frameRendered(); // emitted from the render thread, at most once per frame displayed by the UI

public:
    void acknowledgeFrame() { m_framePending = false; } // the UI has handled frameRendered

private:
    friend class RenderThread;
    friend struct ScopedCurrent;

    struct Garbage; // detached resources, released in the render thread
    void makeCurrent();
    void doneCurrent();
    void renderLoop();
    void runPendingTasks();
    void frame(double dt);
    void present(QWindow *w, QSize px);
    void releaseLayer(Layer &l);
    void releaseAll();
    std::shared_ptr<Garbage> detachSource(Layer &l);
    void releaseGarbage(const std::shared_ptr<Garbage> &g);
    void updateSource(Layer &l, double dt);
    void renderLayer(Layer &l, const IsfRenderContext &rc);
    void processLayer(Layer &l, GLuint tex, int w, int h, bool premultiplied, const IsfRenderContext &rc);
    void renderGroup(Layer &g, const std::vector<Layer *> &members, const IsfRenderContext &rc);
    void compositeLayers(const RenderTarget &target, const std::vector<Layer *> &topToBottom);
    void composite();
    void readSourcePreview();
    void normalizeLocked(); // restores the structure invariants (lock held)
    quint64 newIdLocked();
    void drawQuad();
    void blit(GLuint tex, const RenderTarget &target);
    QString resolvePath(const QJsonObject &o, const QString &projectDir) const;
    QJsonObject layerToJson(const Layer &l, const QString &projectDir) const;
    void layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings);
    double nextDt();
    void applyPublishing();               // render thread
    void publishFrame(const RenderTarget &out);
    void setPublishState(PublishKind k, PublishState st);

    mutable QRecursiveMutex m_mutex;

    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QSurface *m_currentSurface = nullptr;
    QThread *m_thread = nullptr;
    QThread *m_ownerThread = nullptr;
    std::atomic<bool> m_threaded{false}, m_quit{false}, m_framePending{false};

    // OpenGL task queue
    struct Task {
        std::function<void()> fn;
        bool done = false;
    };
    std::mutex m_taskMutex;
    std::condition_variable m_taskCv;
    std::deque<std::shared_ptr<Task>> m_tasks;

    // Output window (read by the render thread)
    QWindow *m_outWindow = nullptr;
    bool m_outExposed = false;
    QSize m_outPixels;

    GLuint m_quadVao = 0, m_quadVbo = 0;
    GLuint m_meshVao = 0, m_meshVbo = 0, m_meshIbo = 0;
    GLsizei m_meshIndexCount = 0;
    GLuint m_blitProgram = 0, m_compProgram = 0, m_presentProgram = 0, m_prepProgram = 0;
    GLint m_blitTexLoc = -1, m_compTexLoc = -1, m_compOpacityLoc = -1, m_presentTexLoc = -1;
    GLint m_prepTexLoc = -1, m_prepCropLoc = -1, m_prepAddLoc = -1, m_prepRemoveLoc = -1, m_prepUnpremulLoc = -1, m_prepBalanceLoc = -1;
    GLuint m_blackTex = 0;
    RenderTarget m_output[2];
    std::atomic<int> m_published{0};
    int m_back = 1;

    QSize m_compSize{1920, 1080};
    std::vector<std::unique_ptr<Layer>> m_layers;
    quint64 m_nextId = 1;
    std::vector<float> m_meshScratch;

    std::atomic<double> m_masterLevel{1.0};
    double m_masterTarget = 1.0, m_masterSpeed = 0.0; // units per second (0 = immediate)
    std::atomic<double> m_blackLevel{1.0};
    double m_blackTarget = 1.0, m_blackSpeed = 0.0, m_blackFade = 1.0;

    // Source preview
    quint64 m_previewId = 0;
    int m_previewSide = 360, m_previewTick = 0;
    RenderTarget m_previewTarget;
    mutable std::mutex m_previewMutex;
    QImage m_previewImage;
    quint64 m_previewImageId = 0;

    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
    double m_realDt = 0; // unclamped frame interval: playheads follow real time, as the sound does
    int64_t m_frameStampNs = 0; // steady_clock time the playheads of the current frame correspond to

    std::unique_ptr<AudioOutput> m_audio;
    PlayMode m_defaultPlayMode = PlayMode::Loop;
    void attachAudio(Layer &l, std::shared_ptr<AudioStream> s);
    std::atomic<double> m_fps{0};
    std::atomic<quint64> m_frameCount{0};
    bool m_initialized = false;

    QString m_projectPath;
    IsfLibrary m_library;
    QStringList m_binItems;

    // Publishing
    PublishSettings m_publish, m_publishApplied;
    bool m_publishDirty = false, m_publishInit = false;
    std::function<void(const CpuFrame &)> m_tap;
    bool m_tapDirty = false;
    std::unique_ptr<GpuPublisher> m_gpuPubs[kPublishKindCount];
    std::shared_ptr<CpuPublisher> m_cpuPubs[kPublishKindCount], m_tapPub;
    std::unique_ptr<CpuSendThread> m_sender;
    mutable std::mutex m_stateMutex;
    PublishState m_states[kPublishKindCount];
    RenderTarget m_readback;
    GLuint m_flipProgram = 0, m_pbo[2] = {0, 0};
    GLint m_flipTexLoc = -1;
    int m_pboIndex = 0, m_pboW = 0, m_pboH = 0;
    bool m_pboPending = false;
    double m_pboTime = 0;
    int m_announcedRate = 0;
    double m_rateSince = 0;
};
