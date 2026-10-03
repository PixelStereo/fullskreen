// Engine: life of the engine itself — OpenGL context and render thread, the runGl() task protocol,
// the output window, the composition size, and the master / blackout fades.
// The rest of its methods live in Layers.cpp, Render.cpp, Project.cpp, Media.cpp and Memories.cpp.
#include "EngineInternal.h"

#include <QDebug>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QThread>
#include <QWindow>
#include <cmath>

// ---------------------------------------------------------------------------
// Render thread, OpenGL context, start and stop
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

// Makes the engine context current for the duration of a scope (manual mode only).
struct ScopedCurrent {
    Engine *e;
    explicit ScopedCurrent(Engine *engine) : e(engine) { e->makeCurrent(); }
    ~ScopedCurrent() { e->doneCurrent(); }
};

Engine::Engine(QObject *parent) : QObject(parent), m_audio(std::make_unique<AudioOutput>()) {}

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
    // Layer preparation: roi (part of the source used), color (added / removed), unpremultiplied alpha (groups)
    m_prepProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_tex; uniform vec4 u_roi; uniform vec3 u_add;\n"
                                   "uniform vec3 u_remove; uniform vec3 u_balance; uniform int u_unpremul; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 c = texture(u_tex, mix(u_roi.xy, u_roi.zw, v_uv));\n"
                                   "  if (u_unpremul != 0 && c.a > 0.0) c.rgb /= c.a;\n"
                                   "  o = vec4(clamp(c.rgb * u_balance * (1.0 - u_remove) + u_add, 0.0, 1.0), c.a); }\n",
                                   &log);
    if (!m_prepProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_prepTexLoc = f->glGetUniformLocation(m_prepProgram, "u_tex");
    m_prepRoiLoc = f->glGetUniformLocation(m_prepProgram, "u_roi");
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

    doneCurrent();

    m_library.scan();
    m_clock.start();
    ensureComposition(); // a composition always has one composition: what the screen shows
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
// OpenGL tasks (runGl)
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

        frame(nextDt());

        // One window per composition: each is made current in turn and swapped (each swap waits for its vsync)
        bool presented = false;
        std::vector<std::pair<Layer *, OutputSurface>> shown;
        {
            Lock lk(&m_mutex);
            for (auto &l : m_layers) {
                if (!l->isComposition) continue;
                auto it = m_outWindows.find(l->id);
                if (it != m_outWindows.end() && it->second.window && it->second.exposed)
                    shown.push_back({l.get(), it->second});
            }
        }
        for (const auto &[vp, out] : shown) {
            if (!m_context->makeCurrent(out.window)) continue;
            m_currentSurface = out.window;
            present(*vp, out.pixels);
            m_context->swapBuffers(out.window);
            presented = true;
        }
        if (m_currentSurface != m_surface) {
            m_context->makeCurrent(m_surface);
            m_currentSurface = m_surface;
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
// Output window
// ---------------------------------------------------------------------------
void Engine::setCompositionWindow(quint64 compositionId, QWindow *w)
{
    runGl([this, compositionId, w] {
        OutputSurface &o = m_outWindows[compositionId];
        if (m_threaded && o.window && m_currentSurface == o.window && o.window != w) {
            m_context->makeCurrent(m_surface);
            m_currentSurface = m_surface;
        }
        o.window = w;
        if (!w) {
            m_outWindows.erase(compositionId);
            return;
        }
        o.exposed = false;
    });
}

void Engine::setCompositionExposed(quint64 compositionId, bool exposed, QSize pixelSize)
{
    runGl([this, compositionId, exposed, pixelSize] {
        auto it = m_outWindows.find(compositionId);
        if (it == m_outWindows.end()) return;
        if (!exposed && m_threaded && m_currentSurface == it->second.window && it->second.window) {
            m_context->makeCurrent(m_surface);
            m_currentSurface = m_surface;
        }
        it->second.exposed = exposed;
        it->second.pixels = pixelSize;
    });
}

// Draws a composition's last finished picture into the window that is current
void Engine::present(const Layer &composition, QSize px)
{
    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_context->defaultFramebufferObject());
    f->glViewport(0, 0, px.width(), px.height());
    f->glDisable(GL_BLEND);
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glUseProgram(m_presentProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, composition.compOut[composition.compPublished.load()].tex);
    f->glUniform1i(m_presentTexLoc, 0);
    drawQuad();
}

quint64 Engine::mainCompositionId() const
{
    Lock lk(&m_mutex);
    for (const auto &l : m_layers)
        if (l->isComposition) return l->id;
    return 0;
}

GLuint Engine::compositionTexture(quint64 compositionId) const
{
    Lock lk(&m_mutex);
    for (const auto &l : m_layers)
        if (l->isComposition && (l->id == compositionId || !compositionId))
            return l->compOut[l->compPublished.load()].tex;
    return 0;
}

GLuint Engine::outputTexture() const { return compositionTexture(0); }

// ---------------------------------------------------------------------------
// Composition size, master and blackout, source preview
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
