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

#include <QColor>
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
#include <map>
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

    // --- Viewports: windows onto the composition, each with its size in pixels, its screen and its
    // publishing. Placed in the composition by their Spatial (mapping bounds); never inside a group.
    int addViewport(const QString &name = {}, QSize size = {}); // index of the new viewport
    bool isViewport(int i) const;
    QList<int> viewports() const;          // indices, in list order (they come first)
    quint64 mainViewportId() const;        // the first one
    void setViewportSize(int i, QSize size);
    void setViewportOutput(int i, const QString &screen, int mode); // mode: 0 hidden, 1 windowed, 2 fullscreen
    // An item at the top of the list can be left out of some viewports (shown everywhere by default)
    void setOpacityIn(int i, quint64 viewport, float opacity); // how much of the item a viewport shows (0..1)
    void ensureViewport(); // there is always at least one

    // JSON snapshot of a layer, and re-creation (undo / redo, duplicate)
    QJsonObject layerJson(int i) const;
    int insertLayerJson(int at, const QJsonObject &o);
    void replaceLayerJson(int i, const QJsonObject &o);
    QJsonArray effectsJson(int i) const;
    void setEffectsJson(int i, const QJsonArray &a);

    // Parts of a layer state (layerJson) that can be copied onto another layer
    enum LayerParts {
        PartSource = 1 << 0,      // media or generator, transport, sound (the media is opened again)
        PartRoi = 1 << 1,         // part of the source picture used
        PartColor = 1 << 2,       // balance, added / removed colors and their switches
        PartSpatial = 1 << 3,     // mapping: corners and mesh
        PartEffects = 1 << 4,     // ISF chain and its master switch
        PartCompositing = 1 << 5, // opacity, blend mode
        PartAll = PartSource | PartRoi | PartColor | PartSpatial | PartEffects | PartCompositing,
    };
    // Copies the chosen parts of `o` (a layerJson, from any layer or project) onto layer i, leaving its
    // identity (id, name, group, place in the list) alone. False if the layer is missing or locked.
    bool applyLayerParts(int i, const QJsonObject &o, int parts);

    bool setLayerVideo(int i, const QString &path, QString *err = nullptr);
    bool setLayerImage(int i, const QString &path, QString *err = nullptr);
    bool setLayerIsf(int i, const QString &path, QString *err = nullptr);
    bool setLayerAudio(int i, const QString &path, QString *err = nullptr);
    // Video, image or audio according to the file: a video file without a picture becomes an audio layer.
    bool setLayerFile(int i, const QString &path, QString *err = nullptr);
    // Another layer of the composition as the source, tapped before or after its effect chain.
    // Refused (with err) for a group, for the layer itself, and whenever the picture would feed back on itself.
    bool setLayerSourceLayer(int i, quint64 sourceId, LayerTap tap, QString *err = nullptr);
    bool setLayerTap(int i, LayerTap tap);
    // True when the picture of layer `id` depends on layer `onId` (source references, members of a group).
    // The lock must be held.
    bool layerDependsOn(quint64 id, quint64 onId) const;
    // Drops the layer sources that point nowhere or would feed back (after a project is read)
    void fixLayerReferences(QStringList *warnings = nullptr);
    void clearLayerSource(int i);
    void setGeneratorSize(int i, int w, int h);

    void setLayerPlaying(int i, bool playing);
    void setLayerPlayMode(int i, PlayMode mode);
    void setLayerSpeed(int i, double speed); // negative: backwards (changing direction keeps the position)
    // In / out points (seconds; out < 0: end of the media). Playback, loops and ping-pong stay within them.
    void setLayerInOut(int i, double in, double out);
    // Color models shown by the Color tab of a new layer (preference); each layer then keeps its own
    void setDefaultColorModels(int m) { m_defaultColorModels = m; }
    // Mode given to a video or a sound when it is loaded into a layer (preference)
    void setDefaultPlayMode(PlayMode m) { m_defaultPlayMode = m; }

    // --- Rendering: frame rate and antialiasing. A project chooses them (Master), or takes the machine's
    // defaults (Settings): -1 in a project means "the default".
    struct RenderSettings {
        double frameRate = -1; // frames per second; 0: the refresh rate of the screen
        int samples = -1;      // multisampling of the mapped edges: 0 / 1 off, 2, 4, 8
        int mipmaps = -1;      // smooth pictures drawn smaller than they are (mipmaps): 0 off, 1 on
        int depth = -1;        // bits per channel of the render: 8 or 10 (kept in 16 bits)
    };
    void setRenderSettings(const RenderSettings &project); // saved with the project
    RenderSettings renderSettings() const;
    void setRenderDefaults(const RenderSettings &machine); // no -1 there
    RenderSettings renderDefaults() const;
    RenderSettings effectiveRender() const; // what is used: the project's, or the defaults
    void setScreenRefreshRate(double hz);   // of the main screen (the pace without a window shown)
    double screenRefreshRate() const;
    // Transition used when a memory gives a layer another source, for the layers that do not choose one
    // (ISF transition; empty: a crossfade)
    void setDefaultTransition(const QString &path);
    QString defaultTransition() const;
    bool isTransitioning(quint64 layer) const; // a source transition is running on it
    // Mask of an effect: the picture of another layer (0: none) says where the effect applies. Refused for a
    // viewport, the layer itself, or a layer that already depends on this one (err says why).
    bool setEffectMask(int layer, int effect, quint64 maskLayer, bool invert, QString *err = nullptr);
    // Where the color section applies: another layer's picture (0: everywhere). Same rules as an effect's mask.
    bool setColorMask(int layer, quint64 maskLayer, bool invert, QString *err = nullptr);
    PlayMode defaultPlayMode() const { return m_defaultPlayMode; }
    void seekLayer(int i, double t);
    void setLayerVolume(int i, float volume);
    void setLayerMuted(int i, bool muted);

    // Text layer properties
    void setLayerSourceText(int i);  // Make layer a text layer
    void setLayerTextContent(int i, const QString &text);
    void setLayerTextFont(int i, const QString &font);
    void setLayerTextSize(int i, int size);
    void setLayerTextColor(int i, const QColor &color);
    void setLayerTextAlign(int i, Qt::Alignment align);

    int addEffect(int layerIndex, const QString &path, QString *err = nullptr);
    void removeEffect(int layerIndex, int fx);
    void moveEffect(int layerIndex, int from, int to);
    bool setIsfImageInput(IsfInstance *inst, int input, const QString &path, QString *err = nullptr);
    bool reloadIsf(IsfInstance *inst); // reloads from disk (live editing)

    // --- Memories (cues, as in MadMapper): snapshots of the layers, recalled with a fade
    struct Memory {
        quint64 id = 0;     // stable (sequences refer to it); given by addMemory
        QString name;
        double fade = 1.0;  // seconds
        QImage thumbnail;   // output at the time it was stored
        QJsonArray layers;  // layerJson of the layers, with "included" (false: left alone by the recall)
    };
    int memoryCount() const;
    Memory memory(int i) const;
    void setMemory(int i, const Memory &m);
    int addMemory(const Memory &m, int at = -1);
    void removeMemory(int i);
    QJsonArray captureLayers() const; // state of every layer, all included
    // Applies layer states (those not excluded): opacity, volume, roi, color, mapping and ISF numbers fade in
    // `fade` seconds, or in the time a state gives them ("timing": key → seconds, 0 a cut); sources and effect chains change at once; a layer that became visible fades in from 0,
    // one that becomes hidden fades out; layers removed since are recreated. Other layers are left alone, or with
    // hideOthers (a memory's recall: the picture as it was stored) faded out and hidden — not the viewports, not
    // the layers the state holds but leaves out ("included": false), not the locked ones.
    void applyLayers(const QJsonArray &layers, double fade, bool hideOthers = false);
    int indexOfMemory(quint64 id) const; // -1: none

    // --- Sequences: ordered steps, each recalling a memory (and carrying a text for the operator), played by
    // GO / GO BACK. Several sequences; one is current. Saved with the project; the position is not.
    struct SequenceStep {
        quint64 memory = 0; // its id (0: none yet)
        QString text;
    };
    struct Sequence {
        QString name;
        bool loop = false; // GO on the last step goes to the first
        std::vector<SequenceStep> steps;
    };
    int sequenceCount() const;
    Sequence sequence(int i) const;
    void setSequence(int i, const Sequence &s);
    int addSequence(const Sequence &s, int at = -1);
    void removeSequence(int i);
    int currentSequence() const;          // -1: none
    void setCurrentSequence(int i);       // its position goes back to before the first step
    int sequencePosition() const;         // step last played in the current sequence (-1: none yet)
    void setSequencePosition(int step);   // without recalling (the interface recalls, undoable)
    int sequenceNext() const;             // the step GO plays (-1: none — the end, without loop)
    int sequencePrevious() const;         // the step GO BACK plays (-1: none)
    // GO / GO BACK / a given step, recalling its memory (OSC; the interface recalls through its undo stack)
    bool sequenceGo();
    bool sequenceBack();
    bool sequenceGoTo(int step);
    void recallMemory(int i); // with its fade
    bool isFading() const;
    // The memory recalled last, and where its fade is (its longest time: values with times of their own included)
    struct RecallProgress {
        quint64 memory = 0;          // 0: none recalled since the project was opened
        double elapsed = 0, total = 0;
        bool running() const { return memory && total > 0 && elapsed < total; }
        double fraction() const { return total > 0 ? std::min(1.0, elapsed / total) : 1.0; }
    };
    RecallProgress recallProgress() const;
    void advanceFades(double dt); // tests: moves the fades on by dt seconds, as a rendered frame does
    void setFadesManual(bool on) { m_fadesManual = on; } // tests: only advanceFades moves them, not the frames
    // Key under which a memory stores the time of a stored value (its path in the layer state), empty for a value
    // that does not fade: "opacity", "roi", "color/temp", "mapping", "effects/0/params/radius"…
    static QString timingKey(const QStringList &path);

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

    // --- Output publishing (NDI, OMT, Syphon, Spout): each viewport publishes its own picture
    void setPublishSettings(quint64 viewport, const PublishSettings &s);
    PublishSettings publishSettings(quint64 viewport) const;
    PublishState publishState(quint64 viewport, PublishKind k) const;
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

    // --- Source preview (roi editor): a downscaled copy of a layer's source picture (before roi),
    // read back by the render thread every few frames while requested.
    void requestSourcePreview(quint64 layerId, int maxSide = 360); // 0: stop
    QImage sourcePreview(quint64 *layerId = nullptr) const;

    // --- Output: window in which the render thread presents the composition
    // One window per viewport; the render thread presents the viewport's picture into it
    void setViewportWindow(quint64 viewport, QWindow *w);
    void setViewportExposed(quint64 viewport, bool exposed, QSize pixelSize);
    GLuint viewportTexture(quint64 viewport) const; // last finished picture of a viewport (shared context)

    // --- Rendering
    void renderFrame();                  // manual mode only
    GLuint outputTexture() const;        // last published frame (readable from a shared context)
    double fps() const { return m_fps.load(); }
    QImage grabOutput();                 // the whole composition (interface preview)
    QImage grabViewport(quint64 viewport);
    QImage grabLayerSource(int index); // a layer's source picture, full size, before its ROI (tests, tools)
    quint64 frameCount() const { return m_frameCount.load(); }
    // Video frames a layer has shown so far (uploaded to the GPU): its playback rate, measured
    quint64 videoFramesShown(int index) const;
    // Compressed texture formats the GPU samples (HAP): DXT, and BC7 / BC6 (not on macOS)
    bool gpuSamplesDxt() const { return m_videoConv.s3tc(); }
    bool gpuSamplesBptc() const { return m_videoConv.bptc(); }

    // --- Project
    void newProject();   // empty, with one viewport
    bool saveProject(const QString &path, const QJsonObject &uiState, QString *err);
    bool loadProject(const QString &path, QJsonObject *uiState, QString *err);
    QString projectPath() const;
    void setProjectPath(const QString &p);

    IsfLibrary &library() { return m_library; }

signals:
    void layersChanged();
    void compositionSizeChanged(QSize size);
    void memoriesChanged();
    void sequencesChanged();        // their contents
    void sequencePositionChanged(); // current sequence or step
    void memoryRecalled(int index);
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
    bool presentViewports(); // every viewport whose window is on screen (false: none)
    void releaseLayer(Layer &l);
    void releaseAll();
    std::shared_ptr<Garbage> detachSource(Layer &l);
    void releaseGarbage(const std::shared_ptr<Garbage> &g);
    void updateSource(Layer &l, double dt);
    void markNeeded(); // which layers are drawn this frame (shown, or used by another one)
    void renderPass(const IsfRenderContext &rc); // every layer, in dependency order
    void renderLayer(Layer &l, const IsfRenderContext &rc);
    void processLayer(Layer &l, GLuint tex, int w, int h, bool premultiplied, const IsfRenderContext &rc);
    void renderGroup(Layer &g, const std::vector<Layer *> &members, const IsfRenderContext &rc);
    // `view`: the part of the composition the target shows (normalized, origin top left)
    // The target is cleared to `clear` first (through a multisampled buffer when antialiasing is on)
    void setSoftEdge(const SoftEdge &se, GLint widthLoc, GLint powerLoc);
    void compositeLayers(const RenderTarget &target, const std::vector<Layer *> &topToBottom,
                         const QRectF &view = QRectF(0, 0, 1, 1), QColor clear = QColor(0, 0, 0, 0),
                         quint64 viewport = 0); // viewport: the one drawn for (its share of each layer's opacity)
    std::map<std::pair<int, int>, MsaaBuffer> m_msaa; // by size (render thread)
    int m_maxSamples = 0;
    void renderViewport(Layer &v, const std::vector<Layer *> &shown, const IsfRenderContext &rc);
    void composite();
    void readSourcePreview();
    void stepFade(double dt); // memory fades (render thread, lock held)
    QJsonObject memoryToJson(const Memory &m, const QString &projectDir) const;
    Memory memoryFromJson(const QJsonObject &o, const QString &projectDir) const;
    void normalizeLocked(); // restores the structure invariants (lock held)
    quint64 newIdLocked();
    void clearProject();
    int viewportCountLocked() const;
    void drawQuad();
    void blit(GLuint tex, const RenderTarget &target);
    void bindMesh(Layer &l);         // its mapped mesh (render thread, mesh vertex array bound)
    void bindMeshBuffer(GLuint vbo); // any mesh vertex buffer
    QString resolvePath(const QJsonObject &o, const QString &projectDir) const;
    QJsonObject layerToJson(const Layer &l, const QString &projectDir) const;
    void layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings);
    double nextDt();
    struct Publication;
    void applyPublishing();               // render thread: every viewport's publishers follow its settings
    void applyPublication(Publication &pub, const PublishSettings &s, bool withTap);
    void publishReadback(Publication &pub, const RenderTarget &out); // NDI, OMT: before the frame's wait
    void publishShared(Publication &pub, const RenderTarget &out);   // Syphon, Spout: after it
    void releasePublication(Publication &pub); // render thread
    void setPublishState(Publication &pub, PublishKind k, PublishState st);

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
    // Each output window has a context of its own, sharing textures and programs with the engine's: its
    // drawable never changes (on macOS, moving one context from window to window every frame left them black).
    struct OutputSurface {
        QWindow *window = nullptr;
        bool exposed = false;
        QSize pixels;
        QOpenGLContext *context = nullptr; // created on first presentation (render thread)
        GLuint vao = 0;                    // vertex arrays are not shared between contexts
    };
    void releaseOutputSurface(OutputSurface &o);
    void present(const Layer &viewport, OutputSurface &out);
    std::map<quint64, OutputSurface> m_outWindows; // by viewport id (render thread)

    GLuint m_quadVao = 0, m_quadVbo = 0;
    GLuint m_meshVao = 0, m_meshVbo = 0, m_meshIbo = 0;
    GLsizei m_meshIndexCount = 0;
    GLuint m_diffProgram = 0, m_blitProgram = 0, m_compProgram = 0, m_presentProgram = 0, m_prepProgram = 0;
    GLint m_compSoftLoc = -1, m_compSoftPowLoc = -1, m_diffSoftLoc = -1, m_diffSoftPowLoc = -1;
    GLint m_diffTexLoc = -1, m_diffDstLoc = -1, m_diffOpacityLoc = -1, m_diffViewLoc = -1;
    RenderTarget m_dstCopy; // what is drawn so far, for Difference
    GLint m_blitTexLoc = -1, m_compTexLoc = -1, m_compOpacityLoc = -1, m_compViewLoc = -1, m_presentTexLoc = -1;
    GLint m_prepTexLoc = -1, m_prepRoiLoc = -1, m_prepAddLoc = -1, m_prepRemoveLoc = -1, m_prepUnpremulLoc = -1, m_prepBalanceLoc = -1;
    GLint m_prepMaskLoc = -1, m_prepMaskModeLoc = -1;
    GLuint m_blackTex = 0;
    VideoConverter m_videoConv; // video frames to RGBA (render thread)
    RenderTarget m_output[2];
    std::atomic<int> m_published{0};
    int m_back = 1;

    QSize m_compSize{1920, 1080};
    std::vector<std::unique_ptr<Layer>> m_layers;
    quint64 m_nextId = 1;
    std::vector<float> m_meshScratch;
    std::vector<uint8_t> m_renderMark; // render pass: layer already rendered this frame (reused, no allocation)

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
    int m_defaultColorModels = 1;
    std::vector<Memory> m_memories;
    struct FadeJob;
    std::vector<std::shared_ptr<FadeJob>> m_fades;
    // A memory gives a layer another source: the outgoing one keeps playing, invisible, and the two pictures
    // are mixed by an ISF transition into the layer's picture (before its mapping) until it is over
    struct SourceTransition {
        std::unique_ptr<Layer> from; // the outgoing source, with its own ROI, color and effects
        QString shaderPath;
        std::unique_ptr<IsfInstance> shader; // loaded by the render thread (none: a crossfade)
        bool loaded = false;
        RenderTarget target;
        double elapsed = 0, duration = 1;
    };
    std::map<quint64, std::unique_ptr<SourceTransition>> m_transitions; // by layer id
    QString m_defaultTransition;
    void startSourceTransition(int index, const QJsonObject &state, double duration);
    void stepTransitions(double dt);                                      // render thread, lock held
    void renderTransition(Layer &l, SourceTransition &t, const IsfRenderContext &rc);
    void retireTransition(std::unique_ptr<SourceTransition> t);           // sound now, OpenGL on the render thread
    double m_fadeElapsed = 0; // seconds since the last recall
    quint64 m_recalledMemory = 0;
    double m_recallTotal = 0;
    quint64 m_nextMemoryId = 1;
    RenderSettings m_render, m_renderDefaults{0, 0, 0};
    double m_screenHz = 60;
    std::vector<Sequence> m_sequences;
    int m_currentSequence = -1, m_sequencePosition = -1;
    QJsonArray sequencesToJson() const;
    void sequencesFromJson(const QJsonArray &a, int current);
    std::atomic<bool> m_fadesManual{false};
    void attachAudio(Layer &l, std::shared_ptr<AudioStream> s);
    std::atomic<double> m_fps{0};
    std::atomic<quint64> m_frameCount{0};
    bool m_initialized = false;

    QString m_projectPath;
    IsfLibrary m_library;
    QStringList m_binItems;

    // Publishing, one set of publishers per viewport (render thread; states read by the interface)
    struct Publication {
        PublishSettings applied;
        bool init = false;
        std::unique_ptr<GpuPublisher> gpu[kPublishKindCount];
        std::shared_ptr<CpuPublisher> cpu[kPublishKindCount], tap;
        std::unique_ptr<CpuSendThread> sender;
        PublishState states[kPublishKindCount]; // guarded by m_stateMutex
        RenderTarget readback;
        GLuint pbo[2] = {0, 0};
        int pboIndex = 0, pboW = 0, pboH = 0;
        bool pboPending = false;
        double pboTime = 0;
        int announcedRate = 0;
        double rateSince = 0;
    };
    std::map<quint64, std::unique_ptr<Publication>> m_pubs; // by viewport id
    bool m_publishDirty = true;
    std::function<void(const CpuFrame &)> m_tap; // tests: frames of the main viewport
    bool m_tapDirty = false;
    mutable std::mutex m_stateMutex;
    GLuint m_flipProgram = 0;
    GLint m_flipTexLoc = -1;
    GLuint m_maskProgram = 0; // an effect's result over its input, through a mask
    GLint m_maskInLoc = -1, m_maskFxLoc = -1, m_maskMaskLoc = -1, m_maskInvertLoc = -1;
};
