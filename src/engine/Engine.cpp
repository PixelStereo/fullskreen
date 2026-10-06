// Engine: life of the engine itself — OpenGL context and render thread, the runGl() task protocol,
// the output window, the composition size, and the composition opacity / blackout fades.
// The rest of its methods live in Layers.cpp, Render.cpp, Project.cpp, Media.cpp and Memories.cpp.
#include "EngineInternal.h"

#include <QDebug>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QThread>
#include <QWindow>
#include <cmath>

#ifdef Q_OS_MACOS
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

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
    { // the whole frame (viewport outputs); each layer has a buffer of its own
        std::vector<float> full;
        Mapping().buildVertices(kMeshSubdiv, full);
        f->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(full.size() * sizeof(float)), full.data(), GL_STATIC_DRAW);
    }
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
        "uniform vec4 u_view;\n" // scale, offset: the part of the composition the target shows
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos*u_view.xy+u_view.zw,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; uniform float u_opacity; in vec2 v_uv; out vec4 o;\n"
        "uniform vec4 u_soft; uniform vec4 u_softPow;\n" // widths and powers: left, right, top, bottom (the picture's top is at v = 1)
        "float softEdge(vec2 uv){ float e = 1.0;\n"
        "  if (u_soft.x > 0.0) e *= pow(clamp(uv.x/u_soft.x, 0.0, 1.0), u_softPow.x);\n"
        "  if (u_soft.y > 0.0) e *= pow(clamp((1.0-uv.x)/u_soft.y, 0.0, 1.0), u_softPow.y);\n"
        "  if (u_soft.z > 0.0) e *= pow(clamp((1.0-uv.y)/u_soft.z, 0.0, 1.0), u_softPow.z);\n"
        "  if (u_soft.w > 0.0) e *= pow(clamp(uv.y/u_soft.w, 0.0, 1.0), u_softPow.w);\n"
        "  return e; }\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); float a = c.a*u_opacity*softEdge(v_uv); o = vec4(c.rgb*a, a); }\n",
        &log);
    // Difference needs what is already drawn, which blending cannot give: the layer reads a copy of it
    m_diffProgram = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; out vec2 v_uv;\n"
        "uniform vec4 u_view;\n"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos*u_view.xy+u_view.zw,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; uniform sampler2D u_dst; uniform float u_opacity; in vec2 v_uv; out vec4 o;\n"
        "uniform vec4 u_soft; uniform vec4 u_softPow;\n"
        "float softEdge(vec2 uv){ float e = 1.0;\n"
        "  if (u_soft.x > 0.0) e *= pow(clamp(uv.x/u_soft.x, 0.0, 1.0), u_softPow.x);\n"
        "  if (u_soft.y > 0.0) e *= pow(clamp((1.0-uv.x)/u_soft.y, 0.0, 1.0), u_softPow.y);\n"
        "  if (u_soft.z > 0.0) e *= pow(clamp((1.0-uv.y)/u_soft.z, 0.0, 1.0), u_softPow.z);\n"
        "  if (u_soft.w > 0.0) e *= pow(clamp(uv.y/u_soft.w, 0.0, 1.0), u_softPow.w);\n"
        "  return e; }\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); float a = c.a*u_opacity*softEdge(v_uv);\n"
        "  vec4 d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0);\n"
        "  o = vec4(abs(d.rgb - c.rgb*a), d.a + a*(1.0-d.a)); }\n",
        &log);
    if (!m_blitProgram || !m_compProgram || !m_presentProgram || !m_diffProgram) {
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
                                   "uniform vec3 u_remove; uniform vec3 u_balance; uniform int u_unpremul;\n"
                                   "uniform sampler2D u_mask; uniform int u_maskMode; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 c = texture(u_tex, mix(u_roi.xy, u_roi.zw, v_uv));\n"
                                   "  if (u_unpremul != 0 && c.a > 0.0) c.rgb /= c.a;\n"
                                   "  vec3 col = clamp(c.rgb * u_balance * (1.0 - u_remove) + u_add, 0.0, 1.0);\n"
                                   // Color through a mask: 1 as it is, 2 inverted (luminance times alpha)
                                   "  if (u_maskMode != 0) { vec4 m = texture(u_mask, v_uv);\n"
                                   "    float k = clamp(dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * m.a, 0.0, 1.0);\n"
                                   "    if (u_maskMode == 2) k = 1.0 - k;\n"
                                   "    col = mix(clamp(c.rgb, 0.0, 1.0), col, k); }\n"
                                   "  o = vec4(col, c.a); }\n",
                                   &log);
    if (!m_prepProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    // Mask of an effect: input and result mixed by the mask's luminance (times its alpha), the mask stretched
    m_maskProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_in; uniform sampler2D u_fx; uniform sampler2D u_mask;\n"
                                   "uniform int u_invert; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 m = texture(u_mask, v_uv);\n"
                                   "  float k = clamp(dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * m.a, 0.0, 1.0);\n"
                                   "  if (u_invert != 0) k = 1.0 - k;\n"
                                   "  o = mix(texture(u_in, v_uv), texture(u_fx, v_uv), k); }\n",
                                   &log);
    if (!m_maskProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_maskInLoc = f->glGetUniformLocation(m_maskProgram, "u_in");
    m_maskFxLoc = f->glGetUniformLocation(m_maskProgram, "u_fx");
    m_maskMaskLoc = f->glGetUniformLocation(m_maskProgram, "u_mask");
    m_maskInvertLoc = f->glGetUniformLocation(m_maskProgram, "u_invert");
    m_prepTexLoc = f->glGetUniformLocation(m_prepProgram, "u_tex");
    m_prepRoiLoc = f->glGetUniformLocation(m_prepProgram, "u_roi");
    m_prepAddLoc = f->glGetUniformLocation(m_prepProgram, "u_add");
    m_prepRemoveLoc = f->glGetUniformLocation(m_prepProgram, "u_remove");
    m_prepUnpremulLoc = f->glGetUniformLocation(m_prepProgram, "u_unpremul");
    m_prepBalanceLoc = f->glGetUniformLocation(m_prepProgram, "u_balance");
    m_prepMaskLoc = f->glGetUniformLocation(m_prepProgram, "u_mask");
    m_prepMaskModeLoc = f->glGetUniformLocation(m_prepProgram, "u_maskMode");
    m_compTexLoc = f->glGetUniformLocation(m_compProgram, "u_tex");
    m_compOpacityLoc = f->glGetUniformLocation(m_compProgram, "u_opacity");
    m_compSoftLoc = f->glGetUniformLocation(m_compProgram, "u_soft");
    m_compSoftPowLoc = f->glGetUniformLocation(m_compProgram, "u_softPow");
    m_diffSoftLoc = f->glGetUniformLocation(m_diffProgram, "u_soft");
    m_diffSoftPowLoc = f->glGetUniformLocation(m_diffProgram, "u_softPow");
    m_diffTexLoc = f->glGetUniformLocation(m_diffProgram, "u_tex");
    m_diffDstLoc = f->glGetUniformLocation(m_diffProgram, "u_dst");
    m_diffOpacityLoc = f->glGetUniformLocation(m_diffProgram, "u_opacity");
    m_diffViewLoc = f->glGetUniformLocation(m_diffProgram, "u_view");
    m_compViewLoc = f->glGetUniformLocation(m_compProgram, "u_view");

    if (!m_videoConv.init(m_quadVao, &log)) {
        if (err) *err = log;
        return false;
    }
    // HAP: what the decoders leave compressed for the GPU, and what they decode themselves
    VideoDecoder::setGpuFormats(m_videoConv.s3tc(), m_videoConv.bptc());
    qInfo().noquote() << "Compressed textures: DXT" << (m_videoConv.s3tc() ? "yes" : "no") << "- BPTC"
                      << (m_videoConv.bptc() ? "yes" : "no (Hap R and Hap HDR decoded on the CPU)");

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
    ensureViewport(); // the composition is always shown through at least one viewport
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
        const bool presented = presentViewports();
        if (!m_framePending.exchange(true)) emit frameRendered();

        // Pace: the frame rate chosen, or the screen's — the vertical sync of the windows when one is shown
        // (it then blocks), the main screen's refresh rate otherwise
        const double rate = effectiveRender().frameRate;
        const double hz = rate > 0 ? rate : screenRefreshRate();
        const qint64 periodUs = qint64(1e6 / std::clamp(hz, 1.0, 1000.0));
        const qint64 us = pace.nsecsElapsed() / 1000;
        if (rate > 0 || !presented || us < 4000) QThread::usleep(static_cast<unsigned long>(std::max<qint64>(0, periodUs - us)));
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
    m_dstCopy.destroy();
    for (auto &[size, ms] : m_msaa) ms.destroy();
    m_msaa.clear();
    // Output windows: their contexts (the windows themselves belong to the interface)
    for (auto &[id, o] : m_outWindows) releaseOutputSurface(o);
    // Source transitions in progress (render thread: released here at once)
    for (auto &[id, t] : m_transitions) retireTransition(std::move(t));
    m_transitions.clear();
    // Publishers: GPU (current context required), then the send threads
    for (auto &[id, pub] : m_pubs) releasePublication(*pub);
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_pubs.clear();
    }
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers) releaseLayer(*l);
        m_layers.clear();
    }
    auto f = gl();
    for (RenderTarget &o : m_output) o.destroy();
    m_previewTarget.destroy();
    m_videoConv.release();
    for (GLuint p : {m_blitProgram, m_compProgram, m_diffProgram, m_presentProgram, m_flipProgram, m_prepProgram, m_maskProgram})
        if (p) f->glDeleteProgram(p);
    GLuint bufs[] = {m_quadVbo, m_meshVbo, m_meshIbo};
    f->glDeleteBuffers(3, bufs);
    GLuint vaos[] = {m_quadVao, m_meshVao};
    f->glDeleteVertexArrays(2, vaos);
    f->glDeleteTextures(1, &m_blackTex);
    m_quadVao = m_meshVao = 0;
    m_blitProgram = m_compProgram = m_diffProgram = m_presentProgram = m_flipProgram = m_prepProgram = m_maskProgram = 0;
}

// ---------------------------------------------------------------------------
// Output window
// ---------------------------------------------------------------------------
void Engine::setViewportWindow(quint64 viewport, QWindow *w)
{
    runGl([this, viewport, w] {
        auto it = m_outWindows.find(viewport);
        if (it != m_outWindows.end() && it->second.window != w) {
            releaseOutputSurface(it->second); // before the window goes
            m_outWindows.erase(it);
        }
        if (w && !m_outWindows.count(viewport)) m_outWindows[viewport] = OutputSurface{w, false, {}};
    });
}

void Engine::setViewportExposed(quint64 viewport, bool exposed, QSize pixelSize)
{
    runGl([this, viewport, exposed, pixelSize] {
        auto it = m_outWindows.find(viewport);
        if (it == m_outWindows.end()) return;
        it->second.exposed = exposed;
        it->second.pixels = pixelSize;
    });
}

// The window's context goes; its vertex array goes with it
void Engine::releaseOutputSurface(OutputSurface &o)
{
    if (o.context && o.vao && o.window && o.context->makeCurrent(o.window)) {
        o.context->extraFunctions()->glDeleteVertexArrays(1, &o.vao);
        o.context->doneCurrent();
    }
    delete o.context;
    o.context = nullptr;
    o.vao = 0;
    m_context->makeCurrent(m_surface);
    m_currentSurface = m_surface;
}

GLuint Engine::viewportTexture(quint64 viewport) const
{
    Lock lk(&m_mutex);
    for (const auto &l : m_layers)
        if (l->isViewport && l->id == viewport) return l->vpOut[l->vpPublished.load()].tex;
    return 0;
}

// Draws a viewport's last finished picture into its window, with the window's context current
void Engine::present(const Layer &viewport, OutputSurface &out)
{
    auto f = out.context->extraFunctions();
    if (!out.vao) { // the shared quad, seen through this context
        f->glGenVertexArrays(1, &out.vao);
        f->glBindVertexArray(out.vao);
        f->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    f->glBindFramebuffer(GL_FRAMEBUFFER, out.context->defaultFramebufferObject());
    f->glViewport(0, 0, out.pixels.width(), out.pixels.height());
    f->glDisable(GL_BLEND);
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glUseProgram(m_presentProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, viewport.vpOut[viewport.vpPublished.load()].tex);
    f->glUniform1i(m_presentTexLoc, 0);
    f->glBindVertexArray(out.vao);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    f->glBindVertexArray(0);
}

// Vertical sync of the swap to come. With several windows, only the last swap of the frame waits for the
// screen: each one waiting in turn would divide the frame rate by the number of windows.
// macOS: a setting of the context; Windows and Linux: of the window that is current.
static void setSwapInterval(QOpenGLContext *ctx, int n)
{
#ifdef Q_OS_MACOS
    Q_UNUSED(ctx);
    const GLint v = n;
    if (CGLContextObj c = CGLGetCurrentContext()) CGLSetParameter(c, kCGLCPSwapInterval, &v);
#elif defined(Q_OS_WIN)
    using Fn = BOOL(WINAPI *)(int);
    static Fn fn = reinterpret_cast<Fn>(ctx->getProcAddress("wglSwapIntervalEXT"));
    if (fn) fn(n);
#else
    using Fn = int (*)(unsigned);
    static Fn fn = reinterpret_cast<Fn>(ctx->getProcAddress("glXSwapIntervalMESA"));
    if (fn) fn(unsigned(n));
#endif
}

// Each window on screen is drawn with its own context and swapped; the engine's context then becomes current
// again. The pictures were finished (glFinish) at the end of the frame, so the other contexts see them whole.
bool Engine::presentViewports()
{
    std::vector<std::pair<const Layer *, OutputSurface *>> shown;
    {
        Lock lk(&m_mutex);
        for (const auto &l : m_layers) {
            if (!l->isViewport) continue;
            auto it = m_outWindows.find(l->id);
            if (it != m_outWindows.end() && it->second.window && it->second.exposed) shown.push_back({l.get(), &it->second});
        }
    }
    bool any = false;
    for (size_t k = 0; k < shown.size(); ++k) {
        const Layer *vp = shown[k].first;
        OutputSurface &out = *shown[k].second;
        if (!out.context) {
            out.context = new QOpenGLContext;
            out.context->setShareContext(m_context);
            out.context->setFormat(m_context->format());
            if (!out.context->create()) {
                qWarning("Output window: unable to create its OpenGL context.");
                delete out.context;
                out.context = nullptr;
                continue;
            }
        }
        if (!out.context->makeCurrent(out.window)) continue;
        present(*vp, out);
        setSwapInterval(out.context, k + 1 == shown.size() ? 1 : 0); // only the last one waits for the screen
        out.context->swapBuffers(out.window);
        any = true;
    }
    if (!shown.empty()) {
        m_context->makeCurrent(m_surface);
        m_currentSurface = m_surface;
    }
    return any;
}

GLuint Engine::outputTexture() const { return m_output[m_published.load()].tex; }

// ---------------------------------------------------------------------------
// Composition size, opacity and blackout, source preview
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

double Engine::compositionOpacityTarget() const
{
    Lock lk(&m_mutex);
    return m_compositionOpacityTarget;
}

void Engine::setAudioVolume(float v)
{
    {
        Lock lk(&m_mutex);
        m_compFade.volume = false;
    }
    m_audio->setVolume(v);
}

void Engine::fadeCompositionOpacity(double target, double seconds)
{
    Lock lk(&m_mutex);
    m_compFade.opacity = false; // the fader takes over from a memory's fade
    m_compositionOpacityTarget = std::clamp(target, 0.0, 1.0);
    m_compositionOpacitySpeed = seconds > 0.0 ? 1.0 / seconds : 0.0;
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
// Rendering settings
// ---------------------------------------------------------------------------
void Engine::setRenderSettings(const RenderSettings &r)
{
    Lock lk(&m_mutex);
    m_render = r;
}

Engine::RenderSettings Engine::renderSettings() const
{
    Lock lk(&m_mutex);
    return m_render;
}

void Engine::setRenderDefaults(const RenderSettings &r)
{
    Lock lk(&m_mutex);
    m_renderDefaults = r;
}

Engine::RenderSettings Engine::renderDefaults() const
{
    Lock lk(&m_mutex);
    return m_renderDefaults;
}

Engine::RenderSettings Engine::effectiveRender() const
{
    Lock lk(&m_mutex);
    RenderSettings e;
    e.frameRate = m_render.frameRate >= 0 ? m_render.frameRate : std::max(0.0, m_renderDefaults.frameRate);
    e.samples = m_render.samples >= 0 ? m_render.samples : std::max(0, m_renderDefaults.samples);
    e.mipmaps = m_render.mipmaps >= 0 ? m_render.mipmaps : std::max(0, m_renderDefaults.mipmaps);
    e.depth = m_render.depth > 0 ? m_render.depth : (m_renderDefaults.depth > 0 ? m_renderDefaults.depth : 8);
    return e;
}

void Engine::setScreenRefreshRate(double hz)
{
    Lock lk(&m_mutex);
    m_screenHz = hz > 1 ? hz : 60.0;
}

double Engine::screenRefreshRate() const
{
    Lock lk(&m_mutex);
    return m_screenHz;
}
