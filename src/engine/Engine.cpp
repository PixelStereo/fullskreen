#include "Engine.h"


#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSaveFile>
#include <QThread>
#include <QWindow>

#include <cmath>
#include <cstring>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif

static constexpr int kMeshSubdiv = 40;

QString blendModeName(BlendMode m)
{
    switch (m) {
    case BlendMode::Add: return QStringLiteral("Add");
    case BlendMode::Screen: return QStringLiteral("Screen");
    case BlendMode::Multiply: return QStringLiteral("Multiply");
    default: return QStringLiteral("Normal");
    }
}

QString blendModeKey(BlendMode m)
{
    switch (m) {
    case BlendMode::Add: return "add";
    case BlendMode::Screen: return "screen";
    case BlendMode::Multiply: return "multiply";
    default: return "normal";
    }
}

QString playModeName(PlayMode m)
{
    switch (m) {
    case PlayMode::OneShot: return QStringLiteral("One-shot");
    case PlayMode::PingPong: return QStringLiteral("Ping-pong");
    case PlayMode::Stop: return QStringLiteral("Stop");
    default: return QStringLiteral("Loop");
    }
}

QString playModeKey(PlayMode m)
{
    switch (m) {
    case PlayMode::OneShot: return "oneshot";
    case PlayMode::PingPong: return "pingpong";
    case PlayMode::Stop: return "stop";
    default: return "loop";
    }
}

PlayMode playModeFromKey(const QString &k, PlayMode fallback)
{
    if (k == "oneshot") return PlayMode::OneShot;
    if (k == "loop") return PlayMode::Loop;
    if (k == "pingpong") return PlayMode::PingPong;
    if (k == "stop") return PlayMode::Stop;
    return fallback;
}

BlendMode blendModeFromKey(const QString &k)
{
    if (k == "add") return BlendMode::Add;
    if (k == "screen") return BlendMode::Screen;
    if (k == "multiply") return BlendMode::Multiply;
    return BlendMode::Normal;
}

// ---------------------------------------------------------------------------
// Render thread, detached resources
// ---------------------------------------------------------------------------

class RenderThread : public QThread
{
public:
    explicit RenderThread(Engine *e) : m_engine(e) { setObjectName("Fulskrin-render"); }

protected:
    void run() override { m_engine->renderLoop(); }

private:
    Engine *m_engine;
};

// Resources removed from the composition (under lock), then released in the render thread.
struct Engine::Garbage {
    std::unique_ptr<VideoDecoder> video;
    std::shared_ptr<AudioStream> audio;
    Texture2D tex;
    std::unique_ptr<IsfInstance> generator;
    RenderTarget generatorTarget;
    std::vector<std::unique_ptr<IsfInstance>> effects;
    std::unique_ptr<Layer> layer;
};

// Makes the engine context current for the duration of a scope (manual mode only).
struct ScopedCurrent {
    Engine *e;
    explicit ScopedCurrent(Engine *engine) : e(engine) { e->makeCurrent(); }
    ~ScopedCurrent() { e->doneCurrent(); }
};

Engine::Engine(QObject *parent) : QObject(parent), m_audio(std::make_unique<AudioOutput>()) {}

// The stream leaves the mix before its decode thread stops.
static void releaseAudio(AudioOutput &out, std::shared_ptr<AudioStream> &a)
{
    if (!a) return;
    out.removeStream(a.get());
    a->close();
    a.reset();
}

// Restarts the layer's clock at `position`, in direction `dir`: decoders follow the new timeline.
static void reposition(Layer &l, double position, int dir)
{
    const double d = l.duration();
    const Timeline t = l.timeline();
    l.origin = d > 0 ? std::clamp(position, t.lo(), t.hi()) : std::max(0.0, position);
    l.dir = dir >= 0 ? 1 : -1;
    l.clock = 0;
    l.ended = false;
    ++l.timelineId;
    if (l.video) {
        l.video->setTimeline(l.timeline());
        l.video->seek(0);
    }
    if (l.audio) l.audio->setTransport(0, l.playing, std::abs(l.speed), l.timeline(), l.timelineId, l.audioGain());
}

// A media starts at its in point — its out point when the speed is negative.
static void startMedia(Layer &l)
{
    l.dir = l.speed < 0 ? -1 : 1;
    const Timeline t = l.timeline();
    reposition(l, l.dir < 0 ? t.hi() : t.lo(), l.dir);
}

void Engine::attachAudio(Layer &l, std::shared_ptr<AudioStream> s)
{
    releaseAudio(*m_audio, l.audio);
    l.audio = std::move(s);
    if (!l.audio) return;
    l.audio->setTransport(l.clock, l.playing, std::abs(l.speed), l.timeline(), l.timelineId, l.audioGain());
    m_audio->addStream(l.audio);
}

bool Engine::startAudio(const QString &device, QString *err, bool nullDevice)
{
    return m_audio->start(device, err, nullDevice);
}

void Engine::stopAudio() { m_audio->stop(); }

Engine::~Engine() { shutdown(); }

void Engine::makeCurrent()
{
    if (m_context) m_context->makeCurrent(m_surface);
    m_currentSurface = m_surface;
}

void Engine::doneCurrent()
{
    if (m_context) m_context->doneCurrent();
    m_currentSurface = nullptr;
}

bool Engine::initialize(QString *err)
{
    m_ownerThread = QThread::currentThread();
    // No parent: the context is moved to the render thread.
    m_context = new QOpenGLContext;
    m_context->setShareContext(QOpenGLContext::globalShareContext());
    m_context->setFormat(QSurfaceFormat::defaultFormat());
    if (!m_context->create()) {
        if (err) *err = QStringLiteral("Unable to create an OpenGL 3.3 context.");
        return false;
    }
    m_surface = new QOffscreenSurface(nullptr, this);
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!m_context->makeCurrent(m_surface)) {
        if (err) *err = QStringLiteral("Unable to make the OpenGL context current.");
        return false;
    }
    const QSurfaceFormat fmt = m_context->format();
    if (fmt.majorVersion() * 10 + fmt.minorVersion() < 33) {
        if (err)
            *err = QStringLiteral("OpenGL 3.3 required (got %1.%2).").arg(fmt.majorVersion()).arg(fmt.minorVersion());
        return false;
    }
    auto f = gl();
    qInfo().noquote() << "OpenGL" << reinterpret_cast<const char *>(f->glGetString(GL_VERSION)) << "-"
                      << reinterpret_cast<const char *>(f->glGetString(GL_RENDERER));

    // Fullscreen quad
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    f->glGenVertexArrays(1, &m_quadVao);
    f->glBindVertexArray(m_quadVao);
    f->glGenBuffers(1, &m_quadVbo);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Mapping mesh: dynamic vertices, fixed indices
    f->glGenVertexArrays(1, &m_meshVao);
    f->glBindVertexArray(m_meshVao);
    f->glGenBuffers(1, &m_meshVbo);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_meshVbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * (kMeshSubdiv + 1) * (kMeshSubdiv + 1), nullptr, GL_DYNAMIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));
    std::vector<GLuint> idx;
    const int n = kMeshSubdiv;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            GLuint a = GLuint(y * (n + 1) + x), b = a + 1, c = a + GLuint(n + 1), d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    f->glGenBuffers(1, &m_meshIbo);
    f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_meshIbo);
    f->glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(GLuint)), idx.data(), GL_STATIC_DRAW);
    m_meshIndexCount = GLsizei(idx.size());
    f->glBindVertexArray(0);

    QString log;
    const char *quadVs = "#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                         "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n";
    m_blitProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ o = texture(u_tex, v_uv); }\n",
                                   &log);
    m_presentProgram = compileProgram(quadVs,
                                      "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                      "void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n",
                                      &log);
    m_compProgram = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; out vec2 v_uv;\n"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; uniform float u_opacity; in vec2 v_uv; out vec4 o;\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); float a = c.a*u_opacity; o = vec4(c.rgb*a, a); }\n",
        &log);
    if (!m_blitProgram || !m_compProgram || !m_presentProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_blitTexLoc = f->glGetUniformLocation(m_blitProgram, "u_tex");
    m_presentTexLoc = f->glGetUniformLocation(m_presentProgram, "u_tex");
    // Vertical flip for readback (NDI / OMT expect top-to-bottom rows)
    m_flipProgram = compileProgram("#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                                   "void main(){ v_uv = vec2(a_pos.x*0.5+0.5, 0.5-a_pos.y*0.5); gl_Position = vec4(a_pos,0.0,1.0); }\n",
                                   "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n",
                                   &log);
    m_flipTexLoc = f->glGetUniformLocation(m_flipProgram, "u_tex");
    // Layer preparation: crop (part of the source used), color (added / removed), unpremultiplied alpha (groups)
    m_prepProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_tex; uniform vec4 u_crop; uniform vec3 u_add;\n"
                                   "uniform vec3 u_remove; uniform vec3 u_balance; uniform int u_unpremul; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 c = texture(u_tex, mix(u_crop.xy, u_crop.zw, v_uv));\n"
                                   "  if (u_unpremul != 0 && c.a > 0.0) c.rgb /= c.a;\n"
                                   "  o = vec4(clamp(c.rgb * u_balance * (1.0 - u_remove) + u_add, 0.0, 1.0), c.a); }\n",
                                   &log);
    if (!m_prepProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_prepTexLoc = f->glGetUniformLocation(m_prepProgram, "u_tex");
    m_prepCropLoc = f->glGetUniformLocation(m_prepProgram, "u_crop");
    m_prepAddLoc = f->glGetUniformLocation(m_prepProgram, "u_add");
    m_prepRemoveLoc = f->glGetUniformLocation(m_prepProgram, "u_remove");
    m_prepUnpremulLoc = f->glGetUniformLocation(m_prepProgram, "u_unpremul");
    m_prepBalanceLoc = f->glGetUniformLocation(m_prepProgram, "u_balance");
    m_compTexLoc = f->glGetUniformLocation(m_compProgram, "u_tex");
    m_compOpacityLoc = f->glGetUniformLocation(m_compProgram, "u_opacity");

    const unsigned char black[4] = {0, 0, 0, 0};
    f->glGenTextures(1, &m_blackTex);
    f->glBindTexture(GL_TEXTURE_2D, m_blackTex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    for (RenderTarget &o : m_output) o.ensure(m_compSize.width(), m_compSize.height());
    doneCurrent();

    m_library.scan();
    m_clock.start();
    m_lastNs = m_clock.nsecsElapsed();
    m_initialized = true;
    return true;
}

bool Engine::start()
{
    if (!m_initialized || m_threaded) return m_threaded;
    if (!QOpenGLContext::supportsThreadedOpenGL()) {
        qWarning("Platform does not support OpenGL rendering on a separate thread: rendering on the UI thread.");
        return false;
    }
    m_quit = false;
    // The context must not be current on any thread before being handed to the render thread.
    if (QOpenGLContext::currentContext() == m_context) doneCurrent();
    m_thread = new RenderThread(this);
    m_context->moveToThread(m_thread);
    m_threaded = true;
    m_thread->start(QThread::HighPriority);
    return true;
}

void Engine::stop()
{
    if (!m_threaded) return;
    m_quit = true;
    m_taskCv.notify_all();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_threaded = false;
}

void Engine::shutdown()
{
    m_audio->stop();
    if (!m_initialized) return;
    stop();
    if (m_quadVao) { // not yet released by the render thread
        ScopedCurrent sc(this);
        releaseAll();
    }
    delete m_context;
    m_context = nullptr;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// OpenGL tasks
// ---------------------------------------------------------------------------

void Engine::runGl(std::function<void()> fn, bool wait)
{
    if (!m_threaded) {
        if (QOpenGLContext::currentContext() == m_context) {
            fn();
        } else {
            ScopedCurrent sc(this);
            fn();
        }
        return;
    }
    if (QThread::currentThread() == m_thread) {
        fn();
        return;
    }
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    std::unique_lock<std::mutex> lk(m_taskMutex);
    m_tasks.push_back(task);
    if (!wait) return;
    m_taskCv.wait(lk, [&] { return task->done || !m_threaded; });
}

void Engine::runPendingTasks()
{
    std::deque<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        tasks.swap(m_tasks);
    }
    if (tasks.empty()) return;
    for (auto &t : tasks) {
        t->fn();
        t->fn = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        for (auto &t : tasks) t->done = true;
    }
    m_taskCv.notify_all();
}

void Engine::renderLoop()
{
    if (!m_context->makeCurrent(m_surface)) {
        qCritical("Render thread: unable to make the OpenGL context current.");
        return;
    }
    m_currentSurface = m_surface;
    QElapsedTimer pace;
    while (!m_quit) {
        pace.start();
        runPendingTasks();

        QSurface *target = (m_outWindow && m_outExposed) ? static_cast<QSurface *>(m_outWindow) : m_surface;
        if (target != m_currentSurface) {
            if (!m_context->makeCurrent(target)) {
                target = m_surface;
                m_context->makeCurrent(m_surface);
            }
            m_currentSurface = target;
        }

        frame(nextDt());

        bool presented = false;
        if (target == m_outWindow && m_outWindow) {
            present(m_outWindow, m_outPixels);
            m_context->swapBuffers(m_outWindow); // blocks until vertical sync
            presented = true;
        }
        if (!m_framePending.exchange(true)) emit frameRendered();

        // No visible output (or vsync does not block): about 60 fps.
        const qint64 us = pace.nsecsElapsed() / 1000;
        if (!presented || us < 4000) QThread::usleep(static_cast<unsigned long>(std::max<qint64>(0, 16667 - us)));
    }
    runPendingTasks();
    releaseAll();
    {
        // Release any callers still waiting
        std::lock_guard<std::mutex> lk(m_taskMutex);
        for (auto &t : m_tasks) t->done = true;
        m_tasks.clear();
    }
    m_taskCv.notify_all();
    m_context->doneCurrent();
    m_currentSurface = nullptr;
    m_context->moveToThread(m_ownerThread);
}

void Engine::releaseAll()
{
    // Publishers: GPU (current context required), then the send thread
    for (auto &p : m_gpuPubs) p.reset();
    if (m_sender) m_sender->setPublishers({});
    m_sender.reset();
    for (auto &p : m_cpuPubs) p.reset();
    m_tapPub.reset();
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers) releaseLayer(*l);
        m_layers.clear();
    }
    auto f = gl();
    for (RenderTarget &o : m_output) o.destroy();
    m_readback.destroy();
    m_previewTarget.destroy();
    if (m_pbo[0]) f->glDeleteBuffers(2, m_pbo);
    m_pbo[0] = m_pbo[1] = 0;
    for (GLuint p : {m_blitProgram, m_compProgram, m_presentProgram, m_flipProgram, m_prepProgram})
        if (p) f->glDeleteProgram(p);
    GLuint bufs[] = {m_quadVbo, m_meshVbo, m_meshIbo};
    f->glDeleteBuffers(3, bufs);
    GLuint vaos[] = {m_quadVao, m_meshVao};
    f->glDeleteVertexArrays(2, vaos);
    f->glDeleteTextures(1, &m_blackTex);
    m_quadVao = m_meshVao = 0;
    m_blitProgram = m_compProgram = m_presentProgram = m_flipProgram = m_prepProgram = 0;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void Engine::setOutputWindow(QWindow *w)
{
    runGl([this, w] {
        if (m_threaded && m_currentSurface == m_outWindow && m_outWindow && m_outWindow != w) {
            m_context->makeCurrent(m_surface);
            m_currentSurface = m_surface;
        }
        m_outWindow = w;
        if (!w) m_outExposed = false;
    });
}

void Engine::setOutputExposed(bool exposed, QSize pixelSize)
{
    runGl([this, exposed, pixelSize] {
        if (!exposed && m_threaded && m_currentSurface == m_outWindow && m_outWindow) {
            m_context->makeCurrent(m_surface);
            m_currentSurface = m_surface;
        }
        m_outExposed = exposed;
        m_outPixels = pixelSize;
    });
}

void Engine::present(QWindow *, QSize px)
{
    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_context->defaultFramebufferObject());
    f->glViewport(0, 0, px.width(), px.height());
    f->glDisable(GL_BLEND);
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glUseProgram(m_presentProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, m_output[m_published].tex);
    f->glUniform1i(m_presentTexLoc, 0);
    drawQuad();
}

GLuint Engine::outputTexture() const { return m_output[m_published.load()].tex; }

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

QSize Engine::compositionSize() const
{
    Lock lk(&m_mutex);
    return m_compSize;
}

void Engine::setCompositionSize(QSize s)
{
    s = s.expandedTo(QSize(16, 16)).boundedTo(QSize(16384, 16384));
    {
        Lock lk(&m_mutex);
        if (s == m_compSize) return;
        m_compSize = s;
    }
    emit compositionSizeChanged(s);
}

int Engine::layerCount() const
{
    Lock lk(&m_mutex);
    return int(m_layers.size());
}

Layer *Engine::layer(int i)
{
    Lock lk(&m_mutex);
    return (i >= 0 && i < int(m_layers.size())) ? m_layers[size_t(i)].get() : nullptr;
}

int Engine::indexOf(const Layer *l) const
{
    Lock lk(&m_mutex);
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].get() == l) return int(i);
    return -1;
}

QString Engine::projectPath() const
{
    Lock lk(&m_mutex);
    return m_projectPath;
}

void Engine::setProjectPath(const QString &p)
{
    Lock lk(&m_mutex);
    m_projectPath = p;
}

int Engine::addLayer(const QString &name, int at)
{
    {
        Lock lk(&m_mutex);
        auto l = std::make_unique<Layer>();
        l->id = newIdLocked();
        l->colorModels = m_defaultColorModels;
        l->name = name.isEmpty() ? QStringLiteral("Layer %1").arg(m_layers.size() + 1) : name;
        l->genWidth = m_compSize.width();
        l->genHeight = m_compSize.height();
        at = std::clamp(at, 0, int(m_layers.size()));
        // Inserted inside a group's block: the new layer joins that group
        if (at > 0 && at < int(m_layers.size())) {
            const Layer &below = *m_layers[size_t(at)];
            if (below.parent) l->parent = below.parent;
        }
        m_layers.insert(m_layers.begin() + at, std::move(l));
    }
    emit layersChanged();
    return at;
}

int Engine::addGroup(const QString &name, int at)
{
    {
        Lock lk(&m_mutex);
        int groups = 0;
        for (const auto &l : m_layers) groups += l->isGroup;
        auto l = std::make_unique<Layer>();
        l->id = newIdLocked();
        l->isGroup = true;
        l->name = name.isEmpty() ? QStringLiteral("Group %1").arg(groups + 1) : name;
        at = std::clamp(at, 0, int(m_layers.size()));
        // A group is never inside a group: placed before the block it would split
        while (at > 0 && at < int(m_layers.size()) && m_layers[size_t(at)]->parent) --at;
        m_layers.insert(m_layers.begin() + at, std::move(l));
    }
    emit layersChanged();
    return at;
}

quint64 Engine::newIdLocked()
{
    for (const auto &l : m_layers) m_nextId = std::max(m_nextId, l->id + 1);
    return m_nextId++;
}

quint64 Engine::layerId(int i) const
{
    Lock lk(&m_mutex);
    return (i >= 0 && i < int(m_layers.size())) ? m_layers[size_t(i)]->id : 0;
}

int Engine::indexOfId(quint64 id) const
{
    Lock lk(&m_mutex);
    if (!id) return -1;
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i]->id == id) return int(i);
    return -1;
}

LayerTree Engine::structure() const
{
    Lock lk(&m_mutex);
    LayerTree t;
    t.reserve(m_layers.size());
    for (const auto &l : m_layers) t.push_back({l->id, l->parent, l->isGroup});
    return t;
}

void Engine::normalizeLocked()
{
    LayerTree t;
    for (const auto &l : m_layers) t.push_back({l->id, l->parent, l->isGroup});
    const LayerTree n = tree::normalized(t);
    if (n == t) return;
    std::vector<std::unique_ptr<Layer>> out;
    out.reserve(m_layers.size());
    for (const TreeNode &node : n) {
        for (auto &l : m_layers) {
            if (l && l->id == node.id) {
                l->parent = node.parent;
                out.push_back(std::move(l));
                break;
            }
        }
    }
    m_layers.swap(out);
}

void Engine::setStructure(const LayerTree &t)
{
    {
        Lock lk(&m_mutex);
        std::vector<std::unique_ptr<Layer>> out;
        out.reserve(m_layers.size());
        for (const TreeNode &node : t) {
            for (auto &l : m_layers) {
                if (l && l->id == node.id) {
                    l->parent = node.parent;
                    out.push_back(std::move(l));
                    break;
                }
            }
        }
        for (auto &l : m_layers) // layers missing from the structure keep their place at the end
            if (l) out.push_back(std::move(l));
        m_layers.swap(out);
        normalizeLocked();
    }
    emit layersChanged();
}

int Engine::groupIndexOf(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size()) || !m_layers[size_t(i)]->parent) return -1;
    return indexOfId(m_layers[size_t(i)]->parent);
}

QList<int> Engine::groupMembers(int g) const
{
    Lock lk(&m_mutex);
    QList<int> out;
    if (g < 0 || g >= int(m_layers.size()) || !m_layers[size_t(g)]->isGroup) return out;
    const quint64 id = m_layers[size_t(g)]->id;
    for (int i = 0; i < int(m_layers.size()); ++i)
        if (m_layers[size_t(i)]->parent == id) out << i;
    return out;
}

bool Engine::isLocked(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size())) return false;
    const Layer &l = *m_layers[size_t(i)];
    if (l.locked) return true;
    const int g = groupIndexOf(i);
    return g >= 0 && m_layers[size_t(g)]->locked;
}

void Engine::releaseLayer(Layer &l)
{
    if (l.video) l.video->close();
    l.video.reset();
    releaseAudio(*m_audio, l.audio);
    l.sourceTex.destroy();
    if (l.generator) l.generator->releaseGl();
    l.generator.reset();
    l.generatorTarget.destroy();
    for (auto &fx : l.effects) fx->releaseGl();
    l.effects.clear();
    l.fxTarget[0].destroy();
    l.fxTarget[1].destroy();
    l.groupTarget.destroy();
    l.prepTarget.destroy();
    l.finalTex = l.rawTex = 0;
}

void Engine::removeLayer(int i)
{
    auto g = std::make_shared<Garbage>();
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_layers.size())) return;
        g->layer = std::move(m_layers[size_t(i)]);
        m_layers.erase(m_layers.begin() + i);
        if (g->layer->isGroup) // its members go back to the top level, where they are
            for (auto &l : m_layers)
                if (l->parent == g->layer->id) l->parent = 0;
        normalizeLocked();
    }
    if (g->layer->video) g->layer->video->close(); // stop the decode thread outside the render thread
    releaseAudio(*m_audio, g->layer->audio);
    runGl([this, g] { releaseLayer(*g->layer); }, false);
    emit layersChanged();
}

void Engine::moveLayer(int from, int to)
{
    {
        Lock lk(&m_mutex);
        const int n = int(m_layers.size());
        if (from < 0 || from >= n) return;
        to = std::clamp(to, 0, n - 1);
        if (from == to) return;
        auto l = std::move(m_layers[size_t(from)]);
        m_layers.erase(m_layers.begin() + from);
        m_layers.insert(m_layers.begin() + to, std::move(l));
        normalizeLocked();
    }
    emit layersChanged();
}

QJsonObject Engine::layerJson(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size())) return {};
    return layerToJson(*m_layers[size_t(i)], QString());
}

int Engine::insertLayerJson(int at, const QJsonObject &o)
{
    const int idx = addLayer(QString(), at);
    layerFromJson(idx, o, QString(), nullptr);
    quint64 id;
    {
        Lock lk(&m_mutex);
        id = m_layers[size_t(idx)]->id;
        normalizeLocked();
    }
    emit layersChanged();
    return indexOfId(id);
}

void Engine::replaceLayerJson(int i, const QJsonObject &o)
{
    if (i < 0 || i >= layerCount()) return;
    const QList<int> members = groupMembers(i);
    std::vector<quint64> memberIds;
    for (int m : members) memberIds.push_back(layerId(m));
    removeLayer(i);
    const int ni = insertLayerJson(i, o);
    if (memberIds.empty()) return;
    {
        Lock lk(&m_mutex);
        const quint64 gid = layerId(ni);
        for (auto &l : m_layers)
            if (std::find(memberIds.begin(), memberIds.end(), l->id) != memberIds.end()) l->parent = gid;
        normalizeLocked();
    }
    emit layersChanged();
}

int Engine::duplicateLayer(int i)
{
    QJsonObject o = layerJson(i);
    if (o.isEmpty()) return -1;
    const QList<int> members = groupMembers(i);
    QList<QJsonObject> mem;
    for (int m : members) mem << layerJson(m);
    o["name"] = o.value("name").toString() + QStringLiteral(" copy");
    o.remove("id"); // the copy gets its own id
    const int gi = insertLayerJson(i, o);
    const quint64 gid = layerId(gi);
    for (int k = 0; k < mem.size(); ++k) {
        QJsonObject m = mem[k];
        m.remove("id");
        m["parent"] = QString::number(gid);
        insertLayerJson(gi + 1 + k, m);
    }
    return gi;
}

QJsonArray Engine::effectsJson(int i) const
{
    Lock lk(&m_mutex);
    QJsonArray a;
    if (i < 0 || i >= int(m_layers.size())) return a;
    for (const auto &e : m_layers[size_t(i)]->effects) a.append(e->save(QString()));
    return a;
}

void Engine::setEffectsJson(int i, const QJsonArray &a)
{
    auto g = std::make_shared<Garbage>();
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_layers.size())) return;
        g->effects = std::move(m_layers[size_t(i)]->effects);
        m_layers[size_t(i)]->effects.clear();
    }
    runGl([g] { for (auto &e : g->effects) e->releaseGl(); }, false);
    for (const QJsonValue &v : a) {
        const QJsonObject e = v.toObject();
        const int fi = addEffect(i, resolvePath(e, QString()));
        if (fi < 0) continue;
        IsfInstance *inst;
        {
            Lock lk(&m_mutex);
            inst = m_layers[size_t(i)]->effects[size_t(fi)].get();
        }
        runGl([inst, e] {
            inst->enabled = e.value("enabled").toBool(true);
            inst->restoreParams(e.value("params").toObject(), QString());
        });
    }
}

std::shared_ptr<Engine::Garbage> Engine::detachSource(Layer &l)
{
    auto g = std::make_shared<Garbage>();
    g->video = std::move(l.video);
    g->audio = std::move(l.audio);
    g->tex = l.sourceTex;
    l.sourceTex = Texture2D{};
    g->generator = std::move(l.generator);
    g->generatorTarget = l.generatorTarget;
    l.generatorTarget = RenderTarget{};
    l.pendingImage = QImage();
    l.type = SourceType::None;
    l.sourcePath.clear();
    l.error.clear();
    l.srcWidth = l.srcHeight = 0;
    l.missingType = SourceType::None;
    l.finalTex = 0;
    return g;
}

// Releases a detached source: decode thread stopped here, GL resources in the render thread.
void Engine::releaseGarbage(const std::shared_ptr<Garbage> &g)
{
    if (g->video) g->video->close();
    releaseAudio(*m_audio, g->audio);
    runGl([g] {
        g->tex.destroy();
        if (g->generator) g->generator->releaseGl();
        g->generatorTarget.destroy();
    }, false);
}

void Engine::clearLayerSource(int i)
{
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return;
        g = detachSource(*l);
    }
    releaseGarbage(g);
}

static bool isDefaultMapping(const Mapping &m)
{
    const QPointF def[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i)
        if (m.corners[i] != def[i]) return false;
    for (const QPointF &o : m.offsets)
        if (!o.isNull()) return false;
    return true;
}

static QString missingMessage(const QString &path, const QString &err)
{
    return QFileInfo::exists(path) ? err : QStringLiteral("File not found: ") + path;
}

static void markMissing(Layer &l, SourceType type, const QString &path, const QString &err)
{
    l.missingType = type;
    l.sourcePath = path;
    l.error = missingMessage(path, err);
}

bool Engine::setLayerVideo(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    auto dec = std::make_unique<VideoDecoder>();
    QString e;
    if (!dec->open(path, &e)) { // opened outside the lock: may take a while
        if (err) *err = e;
        return false;
    }
    auto sound = std::make_shared<AudioStream>(); // the file's sound, if it has any
    if (!sound->open(path, AudioOutput::kSampleRate, nullptr)) sound.reset();
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->mode = m_defaultPlayMode; // a newly loaded video takes the default mode (preferences)
        l->ended = false;
        l->srcWidth = dec->width();
        l->srcHeight = dec->height();
        l->video = std::move(dec);
        l->type = SourceType::Video;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        l->playing = true;
        l->inPoint = 0; // a new media is played whole
        l->outPoint = -1;
        attachAudio(*l, std::move(sound));
        startMedia(*l);
        if (isDefaultMapping(l->mapping) && l->srcHeight > 0)
            l->mapping.fitAspect(double(l->srcWidth) / l->srcHeight, double(m_compSize.width()) / m_compSize.height());
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerFile(int i, const QString &path, QString *err)
{
    if (isImageFile(path)) return setLayerImage(i, path, err);
    if (isAudioFile(path)) return setLayerAudio(i, path, err);
    QString e;
    if (setLayerVideo(i, path, &e)) return true; // video and unknown extensions: FFmpeg decides
    if (AudioStream::probe(path, nullptr) && setLayerAudio(i, path, err)) return true; // sound only
    if (err) *err = e;
    return false;
}

bool Engine::setLayerAudio(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    auto sound = std::make_shared<AudioStream>();
    QString e;
    if (!sound->open(path, AudioOutput::kSampleRate, &e)) {
        if (err) *err = e;
        return false;
    }
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->type = SourceType::Audio;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        l->mode = m_defaultPlayMode;
        l->ended = false;
        l->playing = true;
        l->inPoint = 0; // a new media is played whole
        l->outPoint = -1;
        attachAudio(*l, std::move(sound));
        startMedia(*l);
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerImage(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    QImage img(path);
    if (img.isNull()) {
        if (err) *err = QStringLiteral("Unreadable image: ") + path;
        return false;
    }
    img = img.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->pendingImage = img; // uploaded to the GPU by the render thread
        l->srcWidth = img.width();
        l->srcHeight = img.height();
        l->type = SourceType::Image;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (isDefaultMapping(l->mapping))
            l->mapping.fitAspect(double(img.width()) / img.height(), double(m_compSize.width()) / m_compSize.height());
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerIsf(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    auto inst = std::make_unique<IsfInstance>();
    IsfInstance *raw = inst.get();
    bool ok = false;
    runGl([raw, path, &ok] { ok = raw->load(path); });
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) {
            runGl([raw] { raw->releaseGl(); });
            return false;
        }
        g = detachSource(*l);
        l->type = SourceType::Isf;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        l->error = inst->error();
        l->generator = std::move(inst);
        if (!ok && err) *err = l->error;
    }
    releaseGarbage(g);
    return ok;
}

void Engine::setGeneratorSize(int i, int w, int h)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) {
        l->genWidth = std::clamp(w, 1, 16384);
        l->genHeight = std::clamp(h, 1, 16384);
    }
}

void Engine::setLayerPlaying(int i, bool playing)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l || !l->hasTransport()) return;
    if (playing && l->atEnd()) startMedia(*l); // played to the end: starts again
    l->playing = playing;
    if (playing) l->ended = false;
}

void Engine::setLayerPlayMode(int i, PlayMode mode)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l) return;
    const double pos = l->position();
    l->mode = mode;
    l->ended = false;
    if (l->hasTransport()) reposition(*l, pos, l->dir); // same place, new mode
}

void Engine::setLayerInOut(int i, double in, double out)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l) return;
    const double d = l->duration();
    in = std::max(0.0, in);
    if (d > 0) {
        in = std::min(in, d);
        if (out >= 0) out = std::clamp(out, 0.0, d);
        if (out >= d - 1e-6) out = -1; // up to the end: follows the media
    }
    if (out >= 0 && out < in + 0.01) out = in + 0.01; // at least a few milliseconds
    if (std::abs(l->inPoint - in) < 1e-9 && std::abs(l->outPoint - out) < 1e-9) return;
    const double pos = l->position();
    l->inPoint = in;
    l->outPoint = out;
    if (l->hasTransport()) reposition(*l, pos, l->dir); // clamped into the new range
}

void Engine::setLayerSpeed(int i, double speed)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l) return;
    const int dir = speed < 0 ? -1 : speed > 0 ? 1 : l->dir;
    const bool turn = dir != l->dir;
    const double pos = l->position();
    l->speed = speed;
    if (turn && l->hasTransport()) reposition(*l, pos, dir); // the other way from the same place
}

void Engine::seekLayer(int i, double t)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l || !l->hasTransport()) return;
    reposition(*l, t, l->dir);
}

void Engine::setLayerVolume(int i, float volume)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->volume = std::clamp(volume, 0.0f, 2.0f);
}

void Engine::setLayerMuted(int i, bool muted)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->muted = muted;
}

int Engine::addEffect(int li, const QString &path, QString *err)
{
    if (!layer(li)) return -1;
    auto inst = std::make_unique<IsfInstance>();
    IsfInstance *raw = inst.get();
    runGl([raw, path] { raw->load(path); });
    if (!raw->isValid() && err) *err = raw->error();
    Lock lk(&m_mutex);
    Layer *l = layer(li);
    if (!l) {
        runGl([raw] { raw->releaseGl(); });
        return -1;
    }
    l->effects.push_back(std::move(inst));
    return int(l->effects.size()) - 1;
}

void Engine::removeEffect(int li, int fx)
{
    std::shared_ptr<IsfInstance> victim;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(li);
        if (!l || fx < 0 || fx >= int(l->effects.size())) return;
        victim.reset(l->effects[size_t(fx)].release());
        l->effects.erase(l->effects.begin() + fx);
    }
    runGl([victim] { victim->releaseGl(); }, false);
}

void Engine::moveEffect(int li, int from, int to)
{
    Lock lk(&m_mutex);
    Layer *l = layer(li);
    if (!l) return;
    const int n = int(l->effects.size());
    if (from < 0 || from >= n) return;
    to = std::clamp(to, 0, n - 1);
    if (from == to) return;
    auto e = std::move(l->effects[size_t(from)]);
    l->effects.erase(l->effects.begin() + from);
    l->effects.insert(l->effects.begin() + to, std::move(e));
}

bool Engine::setIsfImageInput(IsfInstance *inst, int input, const QString &path, QString *err)
{
    if (!inst) return false;
    bool ok = false;
    QString e;
    runGl([&] { ok = inst->setImageInput(input, path, &e); });
    if (err) *err = e;
    return ok;
}

bool Engine::reloadIsf(IsfInstance *inst)
{
    if (!inst) return false;
    bool ok = false;
    runGl([&] {
        // Keep parameter values across the reload.
        const QJsonObject saved = inst->save(QString());
        ok = inst->load(inst->path());
        inst->restoreParams(saved.value("params").toObject(), QString());
        inst->enabled = saved.value("enabled").toBool(true);
    });
    Lock lk(&m_mutex);
    for (auto &l : m_layers)
        if (l->generator.get() == inst) l->error = inst->error();
    return ok;
}

double Engine::masterTarget() const
{
    Lock lk(&m_mutex);
    return m_masterTarget;
}

void Engine::fadeMaster(double target, double seconds)
{
    Lock lk(&m_mutex);
    m_masterTarget = std::clamp(target, 0.0, 1.0);
    m_masterSpeed = seconds > 0.0 ? 1.0 / seconds : 0.0;
}

void Engine::setBlackout(bool on, double seconds)
{
    {
        Lock lk(&m_mutex);
        m_blackTarget = on ? 0.0 : 1.0;
        m_blackSpeed = seconds > 0.0 ? 1.0 / seconds : 0.0;
        if (m_blackSpeed <= 0.0) m_blackLevel = m_blackTarget;
    }
    m_audio->fadeTo(on ? 0.0f : 1.0f, seconds);
}

bool Engine::blackout() const
{
    Lock lk(&m_mutex);
    return m_blackTarget < 0.5;
}

void Engine::setBlackoutFade(double seconds)
{
    Lock lk(&m_mutex);
    m_blackFade = std::clamp(seconds, 0.0, 60.0);
}

double Engine::blackoutFade() const
{
    Lock lk(&m_mutex);
    return m_blackFade;
}

void Engine::requestSourcePreview(quint64 layerId, int maxSide)
{
    Lock lk(&m_mutex);
    m_previewId = layerId;
    m_previewSide = std::clamp(maxSide, 16, 2048);
}

QImage Engine::sourcePreview(quint64 *layerId) const
{
    std::lock_guard<std::mutex> lk(m_previewMutex);
    if (layerId) *layerId = m_previewImageId;
    return m_previewImage;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Engine::drawQuad()
{
    auto f = gl();
    f->glBindVertexArray(m_quadVao);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Engine::blit(GLuint tex, const RenderTarget &target)
{
    auto f = gl();
    target.bind();
    f->glDisable(GL_BLEND);
    f->glUseProgram(m_blitProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glUniform1i(m_blitTexLoc, 0);
    drawQuad();
}

void Engine::updateSource(Layer &l, double dt)
{
    if (!l.pendingImage.isNull()) {
        l.sourceTex.upload(l.pendingImage.constBits(), l.pendingImage.width(), l.pendingImage.height());
        l.pendingImage = QImage();
    }
    (void)dt;
    if ((l.type != SourceType::Video && l.type != SourceType::Audio) || !l.hasTransport()) return;
    if (l.playing) {
        // Real time (not the clamped animation step): after a render stall, picture and sound stay together.
        l.clock += m_realDt * std::abs(l.speed);
        if (l.atEnd()) { // One-shot / Stop: end of the media (the end, or the start when playing backwards)
            l.clock = l.timeline().firstLeg().clockEnd();
            l.playing = false;
            l.ended = l.mode == PlayMode::Stop; // Stop: black (and silent); One-shot: last frame
        }
    }
    if (l.audio)
        l.audio->setTransport(l.clock, l.playing, std::abs(l.speed), l.timeline(), l.timelineId, l.audioGain(), m_frameStampNs);
    if (!l.video) return;
    int w = 0, h = 0;
    if (l.video->fetch(l.clock, l.frameBuffer, &w, &h) && w > 0 && h > 0)
        l.sourceTex.upload(l.frameBuffer.data(), w, h);
}

void Engine::renderLayer(Layer &l, const IsfRenderContext &rc)
{
    l.finalTex = 0;
    l.rawTex = 0;
    if (l.isGroup) return;  // rendered from its members (renderGroup)
    if (l.ended) return; // Stop mode, after the end: nothing
    GLuint tex = 0;
    int w = 0, h = 0;
    switch (l.type) {
    case SourceType::Video:
    case SourceType::Image:
        tex = l.sourceTex.tex;
        w = l.sourceTex.w;
        h = l.sourceTex.h;
        break;
    case SourceType::Isf:
        if (l.generator) {
            l.generator->render(rc, 0, 0, 0, l.generatorTarget, l.genWidth, l.genHeight);
            tex = l.generatorTarget.tex;
            w = l.generatorTarget.w;
            h = l.generatorTarget.h;
        }
        break;
    default: break;
    }
    if (!tex) return;
    processLayer(l, tex, w, h, false, rc);
}

// Source picture -> crop and color (when needed) -> effect chain -> l.finalTex
void Engine::processLayer(Layer &l, GLuint tex, int w, int h, bool premultiplied, const IsfRenderContext &rc)
{
    l.rawTex = tex;
    l.rawW = w;
    l.rawH = h;
    QRectF c = l.crop.normalized() & Layer::fullCrop();
    if (c.width() < 1e-4 || c.height() < 1e-4) c = Layer::fullCrop();
    const bool cropped = c != Layer::fullCrop();
    const ColorAdjust col = l.color.effective(); // switches honoured once, here
    if (cropped || !col.isIdentity() || premultiplied) {
        auto f = gl();
        const int cw = std::max(1, int(std::lround(w * c.width()))), ch = std::max(1, int(std::lround(h * c.height())));
        l.prepTarget.ensure(cw, ch);
        l.prepTarget.bind();
        f->glDisable(GL_BLEND);
        f->glUseProgram(m_prepProgram);
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, tex);
        f->glUniform1i(m_prepTexLoc, 0);
        // Crop: top-left origin in the UI, textures in OpenGL convention (origin bottom left)
        f->glUniform4f(m_prepCropLoc, float(c.left()), float(1.0 - c.bottom()), float(c.right()), float(1.0 - c.top()));
        f->glUniform3f(m_prepAddLoc, col.add[0], col.add[1], col.add[2]);
        f->glUniform3f(m_prepRemoveLoc, col.remove[0], col.remove[1], col.remove[2]);
        float gains[3];
        col.balanceGains(gains);
        f->glUniform3f(m_prepBalanceLoc, gains[0], gains[1], gains[2]);
        f->glUniform1i(m_prepUnpremulLoc, premultiplied ? 1 : 0);
        drawQuad();
        tex = l.prepTarget.tex;
        w = cw;
        h = ch;
    } else if (l.prepTarget.fbo) {
        l.prepTarget.destroy();
    }

    if (l.effectsEnabled) {
        int ping = 0;
        for (auto &fx : l.effects) {
            if (!fx->enabled) continue;
            RenderTarget &dst = l.fxTarget[ping];
            fx->render(rc, tex, w, h, dst, w, h);
            tex = dst.tex;
            ping = 1 - ping;
        }
    }
    l.finalTex = tex;
    l.finalW = w;
    l.finalH = h;
}

// A group's picture: its members composited on a transparent canvas the size of the composition
// (premultiplied), then cropped, colored and processed by its effects like any layer.
void Engine::renderGroup(Layer &g, const std::vector<Layer *> &members, const IsfRenderContext &rc)
{
    g.finalTex = 0;
    g.rawTex = 0;
    g.groupTarget.ensure(m_compSize.width(), m_compSize.height());
    g.groupTarget.clear(0, 0, 0, 0);
    compositeLayers(g.groupTarget, members);
    processLayer(g, g.groupTarget.tex, g.groupTarget.w, g.groupTarget.h, true, rc);
}

void Engine::compositeLayers(const RenderTarget &target, const std::vector<Layer *> &layers)
{
    auto f = gl();
    target.bind();
    f->glEnable(GL_BLEND);
    f->glUseProgram(m_compProgram);
    f->glUniform1i(m_compTexLoc, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindVertexArray(m_meshVao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_meshVbo);

    // The first layer is on top: draw from last to first.
    for (int i = int(layers.size()) - 1; i >= 0; --i) {
        Layer &l = *layers[size_t(i)];
        if (!l.visible || !l.finalTex || l.opacity <= 0.0f) continue;
        switch (l.blend) {
        case BlendMode::Add: f->glBlendFunc(GL_ONE, GL_ONE); break;
        case BlendMode::Screen: f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
        case BlendMode::Multiply: f->glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
        default: f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        }
        l.mapping.buildVertices(kMeshSubdiv, m_meshScratch);
        f->glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(m_meshScratch.size() * sizeof(float)), m_meshScratch.data());
        f->glUniform1f(m_compOpacityLoc, l.opacity);
        f->glBindTexture(GL_TEXTURE_2D, l.finalTex);
        f->glDrawElements(GL_TRIANGLES, m_meshIndexCount, GL_UNSIGNED_INT, nullptr);
    }
    f->glDisable(GL_BLEND);
    f->glBindVertexArray(0);
}

void Engine::composite()
{
    auto f = gl();
    RenderTarget &out = m_output[m_back];
    out.ensure(m_compSize.width(), m_compSize.height());
    out.clear(0, 0, 0, 1);
    std::vector<Layer *> top;
    for (auto &l : m_layers)
        if (!l->parent) top.push_back(l.get());
    compositeLayers(out, top);

    // Master and blackout: multiply the whole image by the level (constant blend color).
    const double master = m_masterLevel.load() * m_blackLevel.load();
    if (master < 0.999) {
        f->glEnable(GL_BLEND);
        const float m = float(master);
        f->glBlendColor(m, m, m, 1.0f);
        f->glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR);
        f->glUseProgram(m_blitProgram);
        f->glBindTexture(GL_TEXTURE_2D, m_blackTex);
        f->glUniform1i(m_blitTexLoc, 0);
        drawQuad();
    }
    f->glDisable(GL_BLEND);
    f->glBindVertexArray(0);
}

double Engine::nextDt()
{
    const qint64 now = m_clock.nsecsElapsed();
    double dt = (now - m_lastNs) / 1e9;
    m_lastNs = now;
    m_frameStampNs = AudioStream::clockNs();
    // Render thread: playheads follow real time (up to 2 s), so that after a stall picture and sound stay together.
    // Manual mode (tests, command-line rendering): frames are rendered on demand, the step stays clamped.
    m_realDt = m_threaded ? std::clamp(dt, 0.0, 2.0) : std::clamp(dt, 0.0, 0.25);
    dt = std::clamp(dt, 0.0, 0.25);
    if (dt > 0) m_fps = m_fps.load() * 0.95 + (1.0 / dt) * 0.05;
    return dt;
}

void Engine::frame(double dt)
{
    // In threaded mode, keep serving OpenGL tasks while the UI holds the lock:
    // it may wait on a task while holding it, without deadlock.
    if (m_threaded) {
        while (!m_mutex.tryLock(2)) {
            runPendingTasks();
            if (m_quit) return;
        }
    } else {
        m_mutex.lock();
    }

    // Master
    double lvl = m_masterLevel.load();
    if (m_masterSpeed <= 0.0) {
        lvl = m_masterTarget;
    } else {
        const double step = dt * m_masterSpeed;
        lvl = lvl < m_masterTarget ? std::min(m_masterTarget, lvl + step) : std::max(m_masterTarget, lvl - step);
    }
    m_masterLevel = lvl;
    double black = m_blackLevel.load();
    if (m_blackSpeed <= 0.0) {
        black = m_blackTarget;
    } else {
        const double step = dt * m_blackSpeed;
        black = black < m_blackTarget ? std::min(m_blackTarget, black + step) : std::max(m_blackTarget, black - step);
    }
    m_blackLevel = black;

    IsfRenderContext rc;
    rc.dt = dt;
    rc.blackTex = m_blackTex;
    rc.drawQuad = [this] { drawQuad(); };
    rc.blit = [this](GLuint t, const RenderTarget &rt) { blit(t, rt); };

    // Members of a hidden group are hidden (and silent) too
    {
        const Layer *group = nullptr;
        for (auto &l : m_layers) {
            if (l->isGroup) group = l.get();
            l->parentVisible = !(l->parent && group && group->id == l->parent && !group->visible);
        }
    }
    stepFade(m_realDt);
    for (auto &l : m_layers) updateSource(*l, dt);
    for (auto &l : m_layers) renderLayer(*l, rc);
    for (size_t i = 0; i < m_layers.size(); ++i) {
        Layer &g = *m_layers[i];
        if (!g.isGroup) continue;
        std::vector<Layer *> members;
        for (size_t k = i + 1; k < m_layers.size() && m_layers[k]->parent == g.id; ++k) members.push_back(m_layers[k].get());
        if (g.visible) renderGroup(g, members, rc);
        else g.finalTex = g.rawTex = 0;
    }
    composite();
    readSourcePreview();
    const bool publishChanged = m_publishDirty || m_tapDirty;
    m_mutex.unlock();

    if (publishChanged || !m_publishInit) applyPublishing();
    publishFrame(m_output[m_back]);

    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The preview reads the image from another context: wait for rendering to finish before publishing it.
    f->glFinish();
    m_published = m_back;
    m_back = 1 - m_back;
    ++m_frameCount;
}

void Engine::readSourcePreview()
{
    if (!m_previewId || (m_previewTick++ % 4) != 0) return; // a few times per second is enough
    const Layer *l = nullptr;
    for (auto &x : m_layers)
        if (x->id == m_previewId) l = x.get();
    QImage img;
    if (l && l->rawTex && l->rawW > 0 && l->rawH > 0) {
        const double k = std::min(1.0, double(m_previewSide) / std::max(l->rawW, l->rawH));
        const int w = std::max(1, int(std::lround(l->rawW * k))), h = std::max(1, int(std::lround(l->rawH * k)));
        m_previewTarget.ensure(w, h);
        auto f = gl();
        m_previewTarget.bind();
        f->glDisable(GL_BLEND);
        f->glUseProgram(m_prepProgram); // unpremultiplied (groups), no crop, no color
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, l->rawTex);
        f->glUniform1i(m_prepTexLoc, 0);
        f->glUniform4f(m_prepCropLoc, 0, 0, 1, 1);
        f->glUniform3f(m_prepAddLoc, 0, 0, 0);
        f->glUniform3f(m_prepRemoveLoc, 0, 0, 0);
        f->glUniform3f(m_prepBalanceLoc, 1, 1, 1);
        f->glUniform1i(m_prepUnpremulLoc, l->isGroup ? 1 : 0);
        drawQuad();
        img = QImage(w, h, QImage::Format_RGBA8888);
        f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        f->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
        img = img.mirrored(false, true);
    }
    std::lock_guard<std::mutex> lk(m_previewMutex);
    m_previewImage = img;
    m_previewImageId = m_previewId;
}

void Engine::renderFrame()
{
    if (!m_initialized || m_threaded) return;
    QSurface *target = (m_outWindow && m_outExposed) ? static_cast<QSurface *>(m_outWindow) : m_surface;
    if (!m_context->makeCurrent(target)) {
        target = m_surface;
        m_context->makeCurrent(m_surface);
    }
    m_currentSurface = target;
    frame(nextDt());
    if (target == m_outWindow && m_outWindow) {
        present(m_outWindow, m_outPixels);
        m_context->swapBuffers(m_outWindow);
    }
    if (!m_framePending.exchange(true)) emit frameRendered();
}

QImage Engine::grabOutput()
{
    QImage img;
    runGl([this, &img] {
        const RenderTarget &o = m_output[m_published];
        if (!o.fbo) return;
        img = QImage(o.w, o.h, QImage::Format_RGBA8888);
        auto f = gl();
        f->glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
        f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        f->glReadPixels(0, 0, o.w, o.h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
        f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    });
    return img.mirrored(false, true);
}

// ---------------------------------------------------------------------------
// Project (JSON)
// ---------------------------------------------------------------------------

void Engine::newProject()
{
    std::vector<std::unique_ptr<Layer>> old;
    {
        Lock lk(&m_mutex);
        old.swap(m_layers);
        m_projectPath.clear();
        m_binItems.clear();
        m_memories.clear();
        m_fades.clear();
        m_audio->setMasterVolume(1.0f);
        m_audio->setMuted(false);
        if (m_publish != PublishSettings()) {
            m_publish = PublishSettings();
            m_publishDirty = true;
        }
    }
    for (auto &l : old) {
        if (l->video) l->video->close();
        releaseAudio(*m_audio, l->audio);
        auto g = std::make_shared<Garbage>();
        g->layer = std::move(l);
        runGl([this, g] { releaseLayer(*g->layer); }, false);
    }
    setCompositionSize(QSize(1920, 1080));
    emit layersChanged();
    emit memoriesChanged();
}

QString Engine::resolvePath(const QJsonObject &o, const QString &projectDir) const
{
    const QString abs = o.value("path").toString();
    if (!abs.isEmpty() && QFile::exists(abs)) return abs;
    if (!projectDir.isEmpty()) {
        const QString rel = o.value("relativePath").toString();
        if (!rel.isEmpty()) {
            const QString p = QDir(projectDir).absoluteFilePath(rel);
            if (QFile::exists(p)) return QDir::cleanPath(p);
        }
        const QString same = QDir(projectDir).absoluteFilePath(QFileInfo(abs).fileName());
        if (QFile::exists(same)) return same;
    }
    const QString lib = m_library.findByFileName(QFileInfo(abs).fileName());
    if (!lib.isEmpty()) return lib;
    return abs;
}

QJsonObject Engine::layerToJson(const Layer &l, const QString &projectDir) const
{
    QJsonObject o;
    // Ids are strings: JSON numbers are doubles
    o["id"] = QString::number(l.id);
    if (l.parent) o["parent"] = QString::number(l.parent);
    if (l.isGroup) {
        o["group"] = true;
        o["collapsed"] = l.collapsed;
    }
    o["name"] = l.name;
    o["visible"] = l.visible;
    o["locked"] = l.locked;
    o["opacity"] = l.opacity;
    o["blend"] = blendModeKey(l.blend);
    o["volume"] = l.volume;
    o["muted"] = l.muted;
    QJsonObject src;
    switch (l.type) {
    case SourceType::Video:
        src["type"] = "video";
        src["playMode"] = playModeKey(l.mode);
        src["in"] = l.inPoint;
        src["out"] = l.outPoint;
        src["speed"] = l.speed;
        src["playing"] = l.playing;
        break;
    case SourceType::Audio:
        src["type"] = "audio";
        src["playMode"] = playModeKey(l.mode);
        src["in"] = l.inPoint;
        src["out"] = l.outPoint;
        src["speed"] = l.speed;
        src["playing"] = l.playing;
        break;
    case SourceType::Image: src["type"] = "image"; break;
    case SourceType::Isf:
        src = l.generator ? l.generator->save(projectDir) : QJsonObject();
        src["type"] = "isf";
        src["width"] = l.genWidth;
        src["height"] = l.genHeight;
        break;
    default:
        // Missing file: keep what was intended, so nothing is lost on save.
        if (l.missingType == SourceType::Video || l.missingType == SourceType::Audio) {
            src["type"] = l.missingType == SourceType::Video ? "video" : "audio";
            src["playMode"] = playModeKey(l.mode);
            src["in"] = l.inPoint;
            src["out"] = l.outPoint;
            src["speed"] = l.speed;
            src["playing"] = l.playing;
        } else if (l.missingType == SourceType::Image) {
            src["type"] = "image";
        } else {
            src["type"] = "none";
        }
        break;
    }
    if (l.type != SourceType::None || l.missingType != SourceType::None) {
        src["path"] = l.sourcePath;
        if (!projectDir.isEmpty()) src["relativePath"] = QDir(projectDir).relativeFilePath(l.sourcePath);
    }
    const QRectF c = l.crop;
    src["crop"] = QJsonArray{c.left(), c.top(), c.right(), c.bottom()};
    o["source"] = src;
    auto rgb = [](const float v[3]) { return QJsonArray{v[0], v[1], v[2]}; };
    o["color"] = QJsonObject{{"temp", l.color.temp},        {"tint", l.color.tint},
                             {"add", rgb(l.color.add)},    {"remove", rgb(l.color.remove)},
                             {"enabled", l.color.enabled}, {"tempOn", l.color.tempOn},
                             {"tintOn", l.color.tintOn},   {"addOn", l.color.addOn},
                             {"removeOn", l.color.removeOn}};
    QJsonArray fx;
    for (const auto &e : l.effects) fx.append(e->save(projectDir));
    o["effects"] = fx;
    o["effectsEnabled"] = l.effectsEnabled;
    o["colorModels"] = l.colorModels;
    o["mapping"] = l.mapping.toJson();
    return o;
}

void Engine::layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings)
{
    QString name;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        if (!l) return;
        l->name = o.value("name").toString(l->name);
        l->visible = o.value("visible").toBool(true);
        l->opacity = float(o.value("opacity").toDouble(1.0));
        l->blend = blendModeFromKey(o.value("blend").toString());
        l->volume = float(std::clamp(o.value("volume").toDouble(1.0), 0.0, 2.0));
        l->muted = o.value("muted").toBool(false);
        l->locked = o.value("locked").toBool(false);
        l->isGroup = o.value("group").toBool(false);
        l->collapsed = o.value("collapsed").toBool(false);
        l->effectsEnabled = o.value("effectsEnabled").toBool(true);
        l->colorModels = o.value("colorModels").toInt(l->colorModels);
        // Saved id kept unless another layer already has it
        const quint64 id = o.value("id").toString().toULongLong();
        bool taken = false;
        for (const auto &other : m_layers) taken |= other.get() != l && other->id == id;
        if (id && !taken) {
            l->id = id;
            m_nextId = std::max(m_nextId, id + 1);
        }
        l->parent = o.value("parent").toString().toULongLong();
        const QJsonArray crop = o.value("source").toObject().value("crop").toArray();
        l->crop = crop.size() == 4 ? QRectF(QPointF(crop[0].toDouble(), crop[1].toDouble()),
                                            QPointF(crop[2].toDouble(), crop[3].toDouble())).normalized() & Layer::fullCrop()
                                   : Layer::fullCrop();
        if (l->crop.isEmpty()) l->crop = Layer::fullCrop();
        const QJsonObject color = o.value("color").toObject();
        l->color.temp = float(std::clamp(color.value("temp").toDouble(0), -double(ColorAdjust::kTempRange), double(ColorAdjust::kTempRange)));
        l->color.tint = float(std::clamp(color.value("tint").toDouble(0), -double(ColorAdjust::kTintRange), double(ColorAdjust::kTintRange)));
        for (int c = 0; c < 3; ++c) {
            l->color.add[c] = float(std::clamp(color.value("add").toArray().at(c).toDouble(0), 0.0, 1.0));
            l->color.remove[c] = float(std::clamp(color.value("remove").toArray().at(c).toDouble(0), 0.0, 1.0));
        }
        // Switches: on in a project saved before they existed
        l->color.enabled = color.value("enabled").toBool(true);
        l->color.tempOn = color.value("tempOn").toBool(true);
        l->color.tintOn = color.value("tintOn").toBool(true);
        l->color.addOn = color.value("addOn").toBool(true);
        l->color.removeOn = color.value("removeOn").toBool(true);
        name = l->name;
    }

    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const QString path = type != "none" ? resolvePath(src, projectDir) : QString();
    QString err;
    if (type == "video" || type == "audio") {
        const bool video = type == "video";
        {
            Lock lk(&m_mutex);
            layer(index)->speed = src.value("speed").toDouble(1.0);
        }
        const bool ok = video ? setLayerVideo(index, path, &err) : setLayerAudio(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        // Saved mode ("loop": true / false in projects older than the play modes)
        const PlayMode fallback = src.value("loop").toBool(true) ? PlayMode::Loop : PlayMode::OneShot;
        setLayerPlayMode(index, playModeFromKey(src.value("playMode").toString(), fallback));
        setLayerInOut(index, src.value("in").toDouble(0), src.value("out").toDouble(-1));
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        l->playing = src.value("playing").toBool(true);
        if (l->hasTransport()) startMedia(*l); // saved speed: backwards starts from the end
        if (!ok) markMissing(*l, video ? SourceType::Video : SourceType::Audio, path, err);
    } else if (type == "image") {
        const bool ok = setLayerImage(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        if (!ok) {
            Lock lk(&m_mutex);
            markMissing(*layer(index), SourceType::Image, path, err);
        }
    } else if (type == "isf") {
        {
            Lock lk(&m_mutex);
            layer(index)->genWidth = src.value("width").toInt(m_compSize.width());
            layer(index)->genHeight = src.value("height").toInt(m_compSize.height());
        }
        if (!setLayerIsf(index, path, &err) && warnings) *warnings << name + ": " + err;
        IsfInstance *gen;
        {
            Lock lk(&m_mutex);
            gen = layer(index)->generator.get();
        }
        const QJsonObject params = src.value("params").toObject();
        if (gen) runGl([gen, params, projectDir] { gen->restoreParams(params, projectDir); });
    }

    for (const QJsonValue &v : o.value("effects").toArray()) {
        const QJsonObject e = v.toObject();
        const QString p = resolvePath(e, projectDir);
        const int fi = addEffect(index, p, &err);
        if (fi < 0) continue;
        IsfInstance *inst;
        {
            Lock lk(&m_mutex);
            inst = layer(index)->effects[size_t(fi)].get();
        }
        if (!inst->isValid() && warnings) *warnings << name + " / " + QFileInfo(p).fileName() + ": " + inst->error();
        runGl([inst, e, projectDir] {
            inst->enabled = e.value("enabled").toBool(true);
            inst->restoreParams(e.value("params").toObject(), projectDir);
        });
    }
    // The mapping is restored after the source (which would otherwise auto-adjust the aspect ratio).
    Lock lk(&m_mutex);
    layer(index)->mapping.fromJson(o.value("mapping").toObject());
}

bool Engine::saveProject(const QString &path, const QJsonObject &uiState, QString *err)
{
    const QString dir = QFileInfo(path).absolutePath();
    QJsonObject root;
    {
        Lock lk(&m_mutex);
        root["app"] = "Fulskrin";
        root["formatVersion"] = 1;
        root["composition"] = QJsonObject{{"width", m_compSize.width()}, {"height", m_compSize.height()}};
        QJsonArray layers;
        for (const auto &l : m_layers) layers.append(layerToJson(*l, dir));
        root["layers"] = layers;
        QJsonArray bin;
        for (const QString &p : m_binItems)
            bin.append(QJsonObject{{"path", p}, {"relativePath", QDir(dir).relativeFilePath(p)}});
        root["bin"] = bin;
        root["publish"] = m_publish.toJson();
        root["audio"] = QJsonObject{{"volume", double(m_audio->masterVolume())}, {"muted", m_audio->muted()}};
        QJsonArray mems;
        for (const Memory &m : m_memories) mems.append(memoryToJson(m, dir));
        root["memories"] = mems;
    }
    root["ui"] = uiState;
    // Atomic write: a crash during save does not corrupt the existing file.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

bool Engine::loadProject(const QString &path, QJsonObject *uiState, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("Unreadable project: ") + pe.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    newProject();
    const QJsonObject comp = root.value("composition").toObject();
    setCompositionSize(QSize(comp.value("width").toInt(1920), comp.value("height").toInt(1080)));
    const QString dir = QFileInfo(path).absolutePath();
    QStringList warnings;
    const QJsonArray layers = root.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        int idx = addLayer(QString(), layerCount());
        layerFromJson(idx, layers[i].toObject(), dir, &warnings);
    }
    {
        Lock lk(&m_mutex);
        normalizeLocked();
    }
    QStringList bin;
    for (const QJsonValue &v : root.value("bin").toArray()) bin << resolvePath(v.toObject(), dir);
    addBinItems(bin);
    if (root.contains("publish")) setPublishSettings(PublishSettings::fromJson(root.value("publish").toObject()));
    const QJsonObject audio = root.value("audio").toObject();
    m_audio->setMasterVolume(float(std::clamp(audio.value("volume").toDouble(1.0), 0.0, 2.0)));
    m_audio->setMuted(audio.value("muted").toBool(false));
    {
        Lock lk(&m_mutex);
        for (const QJsonValue &v : root.value("memories").toArray()) m_memories.push_back(memoryFromJson(v.toObject(), dir));
    }
    emit memoriesChanged();
    if (uiState) *uiState = root.value("ui").toObject();
    setProjectPath(QFileInfo(path).absoluteFilePath());
    emit layersChanged();
    if (!warnings.isEmpty() && err) *err = warnings.join('\n');
    return true;
}

// ---------------------------------------------------------------------------
// External media (media bin)
// ---------------------------------------------------------------------------

QStringList Engine::videoExtensions()
{
    static const QStringList e = {"mov", "mp4", "m4v", "avi", "mkv", "webm", "mxf", "mpg", "mpeg", "wmv", "flv", "ts", "hap", "mts", "m2ts"};
    return e;
}

QStringList Engine::imageExtensions()
{
    static const QStringList e = {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "webp", "tga", "exr", "psd"};
    return e;
}

QStringList Engine::audioExtensions()
{
    static const QStringList e = {"wav", "aif", "aiff", "aifc", "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wma", "caf", "w64"};
    return e;
}

bool Engine::isAudioFile(const QString &path) { return audioExtensions().contains(QFileInfo(path).suffix().toLower()); }
bool Engine::isVideoFile(const QString &path) { return videoExtensions().contains(QFileInfo(path).suffix().toLower()); }
bool Engine::isImageFile(const QString &path) { return imageExtensions().contains(QFileInfo(path).suffix().toLower()); }

std::vector<Engine::MediaRef> Engine::mediaUsage() const
{
    std::vector<MediaRef> out;
    enum Kind { ImageKind, VideoKind, AudioKind };
    auto kindOf = [](const QString &p) { return isVideoFile(p) ? VideoKind : isAudioFile(p) ? AudioKind : ImageKind; };
    auto add = [&](const QString &path, Kind kind, const QString &user, bool imported) {
        if (path.isEmpty()) return;
        for (MediaRef &r : out) {
            if (r.path == path) {
                if (!user.isEmpty() && !r.users.contains(user)) r.users << user;
                r.imported |= imported;
                return;
            }
        }
        MediaRef r;
        r.path = path;
        r.video = kind == VideoKind;
        r.audio = kind == AudioKind;
        r.imported = imported;
        if (!user.isEmpty()) r.users << user;
        out.push_back(r);
    };
    {
        Lock lk(&m_mutex);
        for (const auto &l : m_layers) {
            const SourceType t = l->type != SourceType::None ? l->type : l->missingType;
            if (t == SourceType::Video || t == SourceType::Image || t == SourceType::Audio)
                add(l->sourcePath, t == SourceType::Video ? VideoKind : t == SourceType::Audio ? AudioKind : ImageKind,
                    l->name, false);
            auto scan = [&](const IsfInstance *inst, const QString &user) {
                if (!inst) return;
                for (const IsfInput &in : inst->inputs())
                    if (in.type == IsfInput::Image && !in.isInputImage && !in.imagePath.isEmpty())
                        add(in.imagePath, isVideoFile(in.imagePath) ? VideoKind : ImageKind, user, false);
            };
            scan(l->generator.get(), l->name);
            for (const auto &fx : l->effects) scan(fx.get(), l->name + QStringLiteral(" › ") + fx->name());
        }
        for (const QString &p : m_binItems) add(p, kindOf(p), QString(), true);
    }
    for (MediaRef &r : out) r.missing = !QFileInfo::exists(r.path);
    return out;
}

QStringList Engine::binItems() const
{
    Lock lk(&m_mutex);
    return m_binItems;
}

void Engine::addBinItems(const QStringList &paths)
{
    Lock lk(&m_mutex);
    for (const QString &p : paths) {
        if (p.isEmpty()) continue;
        const QString abs = QFileInfo(p).isAbsolute() ? QDir::cleanPath(p) : QFileInfo(p).absoluteFilePath();
        if (!m_binItems.contains(abs)) m_binItems << abs;
    }
}

void Engine::removeBinItem(const QString &path)
{
    Lock lk(&m_mutex);
    m_binItems.removeAll(path);
}

void Engine::relinkBinItem(const QString &from, const QString &to)
{
    Lock lk(&m_mutex);
    const int i = m_binItems.indexOf(from);
    if (i >= 0) {
        if (m_binItems.contains(to)) m_binItems.removeAt(i);
        else m_binItems[i] = to;
    }
}

static bool isfUsesImage(const IsfInstance *inst, const QString &path)
{
    if (!inst) return false;
    for (const IsfInput &in : inst->inputs())
        if (in.type == IsfInput::Image && !in.isInputImage && in.imagePath == path) return true;
    return false;
}

QList<int> Engine::layersUsingMedia(const QString &path) const
{
    QList<int> out;
    Lock lk(&m_mutex);
    for (int i = 0; i < int(m_layers.size()); ++i) {
        const Layer &l = *m_layers[size_t(i)];
        bool uses = l.sourcePath == path && (l.type == SourceType::Video || l.type == SourceType::Image ||
                                             l.type == SourceType::Audio || l.missingType != SourceType::None);
        uses |= isfUsesImage(l.generator.get(), path);
        for (const auto &fx : l.effects) uses |= isfUsesImage(fx.get(), path);
        if (uses) out << i;
    }
    return out;
}

bool Engine::relinkLayerMedia(int i, const QString &from, const QString &to, QString *err)
{
    SourceType kind = SourceType::None;
    Mapping mapping;
    PlayMode mode = PlayMode::Loop;
    double in = 0, out = -1;
    std::vector<std::pair<IsfInstance *, int>> inputs;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        const SourceType t = l->type != SourceType::None ? l->type : l->missingType;
        if (l->sourcePath == from && (t == SourceType::Video || t == SourceType::Image || t == SourceType::Audio)) kind = t;
        mapping = l->mapping;
        mode = l->mode;
        in = l->inPoint;
        out = l->outPoint;
        auto collect = [&](IsfInstance *inst) {
            if (!inst) return;
            for (int k = 0; k < int(inst->inputs().size()); ++k) {
                const IsfInput &in = inst->inputs()[size_t(k)];
                if (in.type == IsfInput::Image && !in.isInputImage && in.imagePath == from) inputs.emplace_back(inst, k);
            }
        };
        collect(l->generator.get());
        for (auto &fx : l->effects) collect(fx.get());
    }
    bool changed = false, ok = true;
    if (kind != SourceType::None) {
        // The new file may be of another type (video replaced by an image, an audio file…)
        SourceType target = kind;
        if (isImageFile(to)) target = SourceType::Image;
        else if (isVideoFile(to)) target = SourceType::Video;
        else if (isAudioFile(to)) target = SourceType::Audio;
        ok = target == SourceType::Video   ? setLayerVideo(i, to, err)
             : target == SourceType::Audio ? setLayerAudio(i, to, err)
                                           : setLayerImage(i, to, err);
        if (ok) {
            setLayerPlayMode(i, mode); // the layer keeps its play mode and its in / out points
            setLayerInOut(i, in, out);
            Lock lk(&m_mutex);
            if (Layer *l = layer(i)) {
                const unsigned rev = l->mapping.revision;
                l->mapping = mapping; // the aligned mapping stays intact
                l->mapping.revision = rev + 1;
            }
            changed = true;
        }
    }
    for (auto &[inst, k] : inputs) {
        QString e;
        if (setIsfImageInput(inst, k, to, &e)) changed = true;
        else if (err && err->isEmpty()) *err = e;
    }
    return changed && ok;
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

void Engine::setPublishSettings(const PublishSettings &s)
{
    Lock lk(&m_mutex);
    if (s == m_publish && m_publishInit) return;
    m_publish = s;
    m_publishDirty = true;
}

PublishSettings Engine::publishSettings() const
{
    Lock lk(&m_mutex);
    return m_publish;
}

void Engine::setTestTap(std::function<void(const CpuFrame &)> fn)
{
    Lock lk(&m_mutex);
    m_tap = std::move(fn);
    m_tapDirty = true;
}

PublishState Engine::publishState(PublishKind k) const
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    return m_states[int(k)];
}

void Engine::setPublishState(PublishKind k, PublishState st)
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    m_states[int(k)] = std::move(st);
}

static bool isGpuKind(PublishKind k) { return k == PublishKind::Syphon || k == PublishKind::Spout; }

void Engine::applyPublishing()
{
    PublishSettings s;
    std::function<void(const CpuFrame &)> tap;
    bool tapDirty;
    {
        Lock lk(&m_mutex);
        s = m_publish;
        m_publishDirty = false;
        tap = m_tap;
        tapDirty = m_tapDirty;
        m_tapDirty = false;
    }
    const bool first = !m_publishInit;
    m_publishInit = true;
    bool cpuChanged = tapDirty;

    for (int ki = 0; ki < kPublishKindCount; ++ki) {
        const PublishKind k = PublishKind(ki);
        const PublishTarget &want = s.targets[ki];
        const PublishTarget &had = m_publishApplied.targets[ki];
        const bool optionsChanged = s.libraryFolder != m_publishApplied.libraryFolder
                                    || (k == PublishKind::Omt && s.omtQuality != m_publishApplied.omtQuality);
        if (!first && want == had && !(want.enabled && optionsChanged)) continue;

        if (isGpuKind(k)) {
            m_gpuPubs[ki].reset();
        } else if (m_cpuPubs[ki]) {
            m_cpuPubs[ki].reset();
            cpuChanged = true;
        }
        if (!want.enabled) {
            setPublishState(k, {PublishState::Off, QStringLiteral("Disabled"), -1});
            continue;
        }
        if (!publishCompiledIn(k)) {
            setPublishState(k, {PublishState::Unavailable,
                                k == PublishKind::Syphon ? QStringLiteral("Syphon is only available on macOS")
                                                         : QStringLiteral("Spout is only available on Windows"),
                                -1});
            continue;
        }
        QString err;
        if (isGpuKind(k)) {
            auto p = k == PublishKind::Syphon ? createSyphonPublisher() : createSpoutPublisher();
            if (p && p->start(want.name, &err)) {
                m_gpuPubs[ki] = std::move(p);
                setPublishState(k, {PublishState::Ok, QStringLiteral("Active: \"%1\"").arg(want.name), -1});
            } else {
                setPublishState(k, {PublishState::Error, err.isEmpty() ? QStringLiteral("Failed to start") : err, -1});
            }
        } else {
            std::shared_ptr<CpuPublisher> p(k == PublishKind::Ndi ? createNdiPublisher() : createOmtPublisher());
            if (p->start(want.name, s, &err)) {
                m_cpuPubs[ki] = p;
                cpuChanged = true;
                setPublishState(k, {PublishState::Ok, QStringLiteral("Active: \"%1\"").arg(want.name), 0});
            } else {
                setPublishState(k, {PublishState::Error, err, -1});
            }
        }
    }
    if (tapDirty) m_tapPub = tap ? std::shared_ptr<CpuPublisher>(createTapPublisher(tap)) : nullptr;
    m_publishApplied = s;

    if (cpuChanged) {
        std::vector<std::shared_ptr<CpuPublisher>> pubs;
        for (auto &p : m_cpuPubs)
            if (p) pubs.push_back(p);
        if (m_tapPub) pubs.push_back(m_tapPub);
        if (!pubs.empty() && !m_sender) m_sender = std::make_unique<CpuSendThread>();
        if (m_sender) m_sender->setPublishers(std::move(pubs));
        m_pboPending = false;
    }
}

static int standardRate(double fps)
{
    static const int rates[] = {24, 25, 30, 48, 50, 60, 72, 75, 90, 100, 120};
    if (fps < 1.0) return 60;
    int best = 60;
    double bestD = 1e9;
    for (int r : rates) {
        const double d = std::fabs(fps - r);
        if (d < bestD) {
            bestD = d;
            best = r;
        }
    }
    return best;
}

void Engine::publishFrame(const RenderTarget &out)
{
    for (int ki : {int(PublishKind::Syphon), int(PublishKind::Spout)})
        if (m_gpuPubs[ki]) m_gpuPubs[ki]->publish(out.tex, out.w, out.h);

    // Receiver count, about twice per second
    if ((m_frameCount.load() % 30) == 0) {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        for (int ki = 0; ki < kPublishKindCount; ++ki) {
            if (m_states[ki].level != PublishState::Ok) continue;
            if (m_cpuPubs[ki]) m_states[ki].receivers = m_cpuPubs[ki]->receivers();
            else if (m_gpuPubs[ki]) m_states[ki].receivers = m_gpuPubs[ki]->receivers();
        }
    }

    if (!m_sender || !m_sender->hasPublishers() || !out.tex) {
        m_pboPending = false;
        return;
    }
    auto f = gl();
    const int w = out.w, h = out.h;
    const GLsizeiptr bytes = GLsizeiptr(w) * h * 4;
    if (!m_pbo[0] || w != m_pboW || h != m_pboH) {
        if (!m_pbo[0]) f->glGenBuffers(2, m_pbo);
        for (GLuint b : m_pbo) {
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
            f->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        m_pboW = w;
        m_pboH = h;
        m_pboPending = false;
    }
    m_readback.ensure(w, h);

    // 1) Current frame flipped (top-to-bottom rows), then read back asynchronously into a PBO
    m_readback.bind();
    f->glDisable(GL_BLEND);
    f->glUseProgram(m_flipProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, out.tex);
    f->glUniform1i(m_flipTexLoc, 0);
    drawQuad();
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_pboIndex]);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

    // 2) Previous frame, already available: copied to the send thread
    if (m_pboPending) {
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[1 - m_pboIndex]);
        const void *ptr = f->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (ptr) {
            if (CpuFrame *cf = m_sender->acquire()) {
                cf->bgra.resize(size_t(bytes));
                std::memcpy(cf->bgra.data(), ptr, size_t(bytes));
                cf->width = w;
                cf->height = h;
                cf->stride = w * 4;
                cf->timestamp100ns = qint64(m_pboTime);
                // Announced frame rate: only changes if the new value holds for 2 s
                const int rate = standardRate(m_fps.load());
                const double nowS = m_clock.nsecsElapsed() / 1e9;
                if (m_announcedRate == 0) m_announcedRate = rate;
                if (rate == m_announcedRate) m_rateSince = nowS;
                else if (nowS - m_rateSince > 2.0) m_announcedRate = rate;
                cf->fpsN = m_announcedRate;
                cf->fpsD = 1;
                m_sender->submit(cf);
            }
            f->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_pboTime = double(m_clock.nsecsElapsed() / 100);
    m_pboPending = true;
    m_pboIndex = 1 - m_pboIndex;
}
