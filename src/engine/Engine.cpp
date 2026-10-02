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
    case BlendMode::Add: return QStringLiteral("Addition");
    case BlendMode::Screen: return QStringLiteral("Écran");
    case BlendMode::Multiply: return QStringLiteral("Produit");
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

BlendMode blendModeFromKey(const QString &k)
{
    if (k == "add") return BlendMode::Add;
    if (k == "screen") return BlendMode::Screen;
    if (k == "multiply") return BlendMode::Multiply;
    return BlendMode::Normal;
}

// ---------------------------------------------------------------------------
// Fil de rendu, ressources détachées
// ---------------------------------------------------------------------------

class RenderThread : public QThread
{
public:
    explicit RenderThread(Engine *e) : m_engine(e) { setObjectName("Lanterne-rendu"); }

protected:
    void run() override { m_engine->renderLoop(); }

private:
    Engine *m_engine;
};

// Ressources retirées de la composition (sous verrou), libérées ensuite dans le fil de rendu.
struct Engine::Garbage {
    std::unique_ptr<VideoDecoder> video;
    Texture2D tex;
    std::unique_ptr<IsfInstance> generator;
    RenderTarget generatorTarget;
    std::vector<std::unique_ptr<IsfInstance>> effects;
    std::unique_ptr<Layer> layer;
};

// Rend le contexte du moteur courant le temps d'un bloc (mode manuel uniquement).
struct ScopedCurrent {
    Engine *e;
    explicit ScopedCurrent(Engine *engine) : e(engine) { e->makeCurrent(); }
    ~ScopedCurrent() { e->doneCurrent(); }
};

Engine::Engine(QObject *parent) : QObject(parent) {}

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
    // Pas de parent : le contexte est déplacé dans le fil de rendu.
    m_context = new QOpenGLContext;
    m_context->setShareContext(QOpenGLContext::globalShareContext());
    m_context->setFormat(QSurfaceFormat::defaultFormat());
    if (!m_context->create()) {
        if (err) *err = QStringLiteral("Impossible de créer un contexte OpenGL 3.3.");
        return false;
    }
    m_surface = new QOffscreenSurface(nullptr, this);
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!m_context->makeCurrent(m_surface)) {
        if (err) *err = QStringLiteral("Impossible d'activer le contexte OpenGL.");
        return false;
    }
    const QSurfaceFormat fmt = m_context->format();
    if (fmt.majorVersion() * 10 + fmt.minorVersion() < 33) {
        if (err)
            *err = QStringLiteral("OpenGL 3.3 requis (obtenu %1.%2).").arg(fmt.majorVersion()).arg(fmt.minorVersion());
        return false;
    }
    auto f = gl();
    qInfo().noquote() << "OpenGL" << reinterpret_cast<const char *>(f->glGetString(GL_VERSION)) << "-"
                      << reinterpret_cast<const char *>(f->glGetString(GL_RENDERER));

    // Quad plein écran
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    f->glGenVertexArrays(1, &m_quadVao);
    f->glBindVertexArray(m_quadVao);
    f->glGenBuffers(1, &m_quadVbo);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Maillage de mapping : sommets dynamiques, indices fixes
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
        if (err) *err = QStringLiteral("Shaders internes : ") + log;
        return false;
    }
    m_blitTexLoc = f->glGetUniformLocation(m_blitProgram, "u_tex");
    m_presentTexLoc = f->glGetUniformLocation(m_presentProgram, "u_tex");
    // Retournement vertical pour la relecture (NDI / OMT attendent des lignes de haut en bas)
    m_flipProgram = compileProgram("#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                                   "void main(){ v_uv = vec2(a_pos.x*0.5+0.5, 0.5-a_pos.y*0.5); gl_Position = vec4(a_pos,0.0,1.0); }\n",
                                   "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n",
                                   &log);
    m_flipTexLoc = f->glGetUniformLocation(m_flipProgram, "u_tex");
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
        qWarning("La plateforme ne permet pas le rendu OpenGL dans un fil séparé : rendu dans le fil de l'interface.");
        return false;
    }
    m_quit = false;
    // Le contexte ne doit être courant dans aucun fil avant d'être confié au fil de rendu.
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
    if (!m_initialized) return;
    stop();
    if (m_quadVao) { // pas encore libéré par le fil de rendu
        ScopedCurrent sc(this);
        releaseAll();
    }
    delete m_context;
    m_context = nullptr;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Tâches OpenGL
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
        qCritical("Fil de rendu : impossible d'activer le contexte OpenGL.");
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
            m_context->swapBuffers(m_outWindow); // bloque jusqu'à la synchro verticale
            presented = true;
        }
        if (!m_framePending.exchange(true)) emit frameRendered();

        // Sans sortie visible (ou si la synchro ne bloque pas) : environ 60 images/s.
        const qint64 us = pace.nsecsElapsed() / 1000;
        if (!presented || us < 4000) QThread::usleep(static_cast<unsigned long>(std::max<qint64>(0, 16667 - us)));
    }
    runPendingTasks();
    releaseAll();
    {
        // Libère d'éventuels appels encore en attente
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
    // Publications : GPU (contexte courant requis) puis fil d'envoi
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
    if (m_pbo[0]) f->glDeleteBuffers(2, m_pbo);
    m_pbo[0] = m_pbo[1] = 0;
    for (GLuint p : {m_blitProgram, m_compProgram, m_presentProgram, m_flipProgram})
        if (p) f->glDeleteProgram(p);
    GLuint bufs[] = {m_quadVbo, m_meshVbo, m_meshIbo};
    f->glDeleteBuffers(3, bufs);
    GLuint vaos[] = {m_quadVao, m_meshVao};
    f->glDeleteVertexArrays(2, vaos);
    f->glDeleteTextures(1, &m_blackTex);
    m_quadVao = m_meshVao = 0;
    m_blitProgram = m_compProgram = m_presentProgram = m_flipProgram = 0;
}

// ---------------------------------------------------------------------------
// Sortie
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
// Calques
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
        l->name = name.isEmpty() ? QStringLiteral("Calque %1").arg(m_layers.size() + 1) : name;
        l->genWidth = m_compSize.width();
        l->genHeight = m_compSize.height();
        at = std::clamp(at, 0, int(m_layers.size()));
        m_layers.insert(m_layers.begin() + at, std::move(l));
    }
    emit layersChanged();
    return at;
}

void Engine::releaseLayer(Layer &l)
{
    if (l.video) l.video->close();
    l.video.reset();
    l.sourceTex.destroy();
    if (l.generator) l.generator->releaseGl();
    l.generator.reset();
    l.generatorTarget.destroy();
    for (auto &fx : l.effects) fx->releaseGl();
    l.effects.clear();
    l.fxTarget[0].destroy();
    l.fxTarget[1].destroy();
    l.finalTex = 0;
}

void Engine::removeLayer(int i)
{
    auto g = std::make_shared<Garbage>();
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_layers.size())) return;
        g->layer = std::move(m_layers[size_t(i)]);
        m_layers.erase(m_layers.begin() + i);
    }
    if (g->layer->video) g->layer->video->close(); // arrêt du fil de décodage hors du fil de rendu
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
    emit layersChanged();
    return idx;
}

void Engine::replaceLayerJson(int i, const QJsonObject &o)
{
    if (i < 0 || i >= layerCount()) return;
    removeLayer(i);
    insertLayerJson(i, o);
}

int Engine::duplicateLayer(int i)
{
    QJsonObject o = layerJson(i);
    if (o.isEmpty()) return -1;
    o["name"] = o.value("name").toString() + QStringLiteral(" copie");
    return insertLayerJson(i, o);
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

// Libère une source détachée : fil de décodage arrêté ici, ressources GL dans le fil de rendu.
void Engine::releaseGarbage(const std::shared_ptr<Garbage> &g)
{
    if (g->video) g->video->close();
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
    return QFileInfo::exists(path) ? err : QStringLiteral("Fichier introuvable : ") + path;
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
    if (!dec->open(path, &e)) { // ouverture hors verrou : peut prendre du temps
        if (err) *err = e;
        return false;
    }
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        dec->setLoop(l->loop);
        l->srcWidth = dec->width();
        l->srcHeight = dec->height();
        l->video = std::move(dec);
        l->type = SourceType::Video;
        l->sourcePath = QFileInfo(path).absoluteFilePath();
        l->playhead = 0;
        l->playing = true;
        if (isDefaultMapping(l->mapping) && l->srcHeight > 0)
            l->mapping.fitAspect(double(l->srcWidth) / l->srcHeight, double(m_compSize.width()) / m_compSize.height());
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerImage(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    QImage img(path);
    if (img.isNull()) {
        if (err) *err = QStringLiteral("Image illisible : ") + path;
        return false;
    }
    img = img.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->pendingImage = img; // envoyé au GPU par le fil de rendu
        l->srcWidth = img.width();
        l->srcHeight = img.height();
        l->type = SourceType::Image;
        l->sourcePath = QFileInfo(path).absoluteFilePath();
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
        l->sourcePath = QFileInfo(path).absoluteFilePath();
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
    if (!l || !l->video) return;
    if (playing && !l->loop && l->duration() > 0 && l->playhead >= l->duration() - 1e-3) seekLayer(i, 0);
    l->playing = playing;
}

void Engine::setLayerLoop(int i, bool loop)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l) return;
    const double pos = l->position();
    l->loop = loop;
    if (l->video) {
        l->video->setLoop(loop);
        if (!loop) seekLayer(i, pos); // ramène l'horloge monotone dans [0, durée]
    }
}

void Engine::seekLayer(int i, double t)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l || !l->video) return;
    const double d = l->duration();
    if (d > 0) t = std::clamp(t, 0.0, d);
    l->playhead = t;
    l->video->seek(t);
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
        // On conserve les valeurs des paramètres à travers le rechargement.
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

// ---------------------------------------------------------------------------
// Rendu
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
    if (l.type != SourceType::Video || !l.video) return;
    if (l.playing) {
        l.playhead += dt * l.speed;
        const double d = l.duration();
        if (!l.loop && d > 0 && l.playhead >= d) {
            l.playhead = d;
            l.playing = false;
        }
    }
    int w = 0, h = 0;
    if (l.video->fetch(l.playhead, l.frameBuffer, &w, &h) && w > 0 && h > 0)
        l.sourceTex.upload(l.frameBuffer.data(), w, h);
}

void Engine::renderLayer(Layer &l, const IsfRenderContext &rc)
{
    l.finalTex = 0;
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

    int ping = 0;
    for (auto &fx : l.effects) {
        if (!fx->enabled) continue;
        RenderTarget &dst = l.fxTarget[ping];
        fx->render(rc, tex, w, h, dst, w, h);
        tex = dst.tex;
        ping = 1 - ping;
    }
    l.finalTex = tex;
    l.finalW = w;
    l.finalH = h;
}

void Engine::composite()
{
    auto f = gl();
    RenderTarget &out = m_output[m_back];
    out.ensure(m_compSize.width(), m_compSize.height());
    out.clear(0, 0, 0, 1);
    f->glEnable(GL_BLEND);
    f->glUseProgram(m_compProgram);
    f->glUniform1i(m_compTexLoc, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindVertexArray(m_meshVao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_meshVbo);

    // Le calque d'index 0 est au-dessus : on dessine du dernier au premier.
    for (int i = int(m_layers.size()) - 1; i >= 0; --i) {
        Layer &l = *m_layers[size_t(i)];
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

    // Master : multiplie toute l'image par le niveau (couleur constante de mélange).
    const double master = m_masterLevel.load();
    if (master < 0.999) {
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
    dt = std::clamp(dt, 0.0, 0.25);
    if (dt > 0) m_fps = m_fps.load() * 0.95 + (1.0 / dt) * 0.05;
    return dt;
}

void Engine::frame(double dt)
{
    // En mode fil, on continue de servir les tâches OpenGL tant que l'interface tient le verrou :
    // elle peut attendre une tâche en le tenant, sans interblocage.
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

    IsfRenderContext rc;
    rc.dt = dt;
    rc.blackTex = m_blackTex;
    rc.drawQuad = [this] { drawQuad(); };
    rc.blit = [this](GLuint t, const RenderTarget &rt) { blit(t, rt); };

    for (auto &l : m_layers) updateSource(*l, dt);
    for (auto &l : m_layers) renderLayer(*l, rc);
    composite();
    const bool publishChanged = m_publishDirty || m_tapDirty;
    m_mutex.unlock();

    if (publishChanged || !m_publishInit) applyPublishing();
    publishFrame(m_output[m_back]);

    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // L'aperçu lit l'image depuis un autre contexte : on attend la fin du rendu avant de la publier.
    f->glFinish();
    m_published = m_back;
    m_back = 1 - m_back;
    ++m_frameCount;
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
// Projet (JSON)
// ---------------------------------------------------------------------------

void Engine::newProject()
{
    std::vector<std::unique_ptr<Layer>> old;
    {
        Lock lk(&m_mutex);
        old.swap(m_layers);
        m_projectPath.clear();
        m_binItems.clear();
        if (m_publish != PublishSettings()) {
            m_publish = PublishSettings();
            m_publishDirty = true;
        }
    }
    for (auto &l : old) {
        if (l->video) l->video->close();
        auto g = std::make_shared<Garbage>();
        g->layer = std::move(l);
        runGl([this, g] { releaseLayer(*g->layer); }, false);
    }
    setCompositionSize(QSize(1920, 1080));
    emit layersChanged();
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
    o["name"] = l.name;
    o["visible"] = l.visible;
    o["opacity"] = l.opacity;
    o["blend"] = blendModeKey(l.blend);
    QJsonObject src;
    switch (l.type) {
    case SourceType::Video:
        src["type"] = "video";
        src["loop"] = l.loop;
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
        // Fichier introuvable : on conserve ce qui était prévu, pour ne rien perdre à l'enregistrement.
        if (l.missingType == SourceType::Video) {
            src["type"] = "video";
            src["loop"] = l.loop;
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
    o["source"] = src;
    QJsonArray fx;
    for (const auto &e : l.effects) fx.append(e->save(projectDir));
    o["effects"] = fx;
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
        name = l->name;
    }

    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const QString path = type != "none" ? resolvePath(src, projectDir) : QString();
    QString err;
    if (type == "video") {
        {
            Lock lk(&m_mutex);
            layer(index)->loop = src.value("loop").toBool(true);
            layer(index)->speed = src.value("speed").toDouble(1.0);
        }
        const bool ok = setLayerVideo(index, path, &err);
        if (!ok && warnings) *warnings << name + " : " + missingMessage(path, err);
        Lock lk(&m_mutex);
        layer(index)->playing = src.value("playing").toBool(true);
        if (!ok) markMissing(*layer(index), SourceType::Video, path, err);
    } else if (type == "image") {
        const bool ok = setLayerImage(index, path, &err);
        if (!ok && warnings) *warnings << name + " : " + missingMessage(path, err);
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
        if (!setLayerIsf(index, path, &err) && warnings) *warnings << name + " : " + err;
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
        if (!inst->isValid() && warnings) *warnings << name + " / " + QFileInfo(p).fileName() + " : " + inst->error();
        runGl([inst, e, projectDir] {
            inst->enabled = e.value("enabled").toBool(true);
            inst->restoreParams(e.value("params").toObject(), projectDir);
        });
    }
    // Le mapping est restauré après la source (qui ajuste sinon le ratio automatiquement).
    Lock lk(&m_mutex);
    layer(index)->mapping.fromJson(o.value("mapping").toObject());
}

bool Engine::saveProject(const QString &path, const QJsonObject &uiState, QString *err)
{
    const QString dir = QFileInfo(path).absolutePath();
    QJsonObject root;
    {
        Lock lk(&m_mutex);
        root["app"] = "Lanterne";
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
    }
    root["ui"] = uiState;
    // Écriture atomique : un plantage pendant l'enregistrement ne corrompt pas le fichier existant.
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
        if (err) *err = QStringLiteral("Projet illisible : ") + pe.errorString();
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
    QStringList bin;
    for (const QJsonValue &v : root.value("bin").toArray()) bin << resolvePath(v.toObject(), dir);
    addBinItems(bin);
    if (root.contains("publish")) setPublishSettings(PublishSettings::fromJson(root.value("publish").toObject()));
    if (uiState) *uiState = root.value("ui").toObject();
    setProjectPath(QFileInfo(path).absoluteFilePath());
    emit layersChanged();
    if (!warnings.isEmpty() && err) *err = warnings.join('\n');
    return true;
}

// ---------------------------------------------------------------------------
// Médias externes (chutier)
// ---------------------------------------------------------------------------

static const QStringList &videoExtensions()
{
    static const QStringList e = {"mov", "mp4", "m4v", "avi", "mkv", "webm", "mxf", "mpg", "mpeg", "wmv", "flv", "ts", "hap", "mts", "m2ts"};
    return e;
}

static const QStringList &imageExtensions()
{
    static const QStringList e = {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "webp", "tga", "exr", "psd"};
    return e;
}

bool Engine::isVideoFile(const QString &path) { return videoExtensions().contains(QFileInfo(path).suffix().toLower()); }
bool Engine::isImageFile(const QString &path) { return imageExtensions().contains(QFileInfo(path).suffix().toLower()); }

std::vector<Engine::MediaRef> Engine::mediaUsage() const
{
    std::vector<MediaRef> out;
    auto add = [&](const QString &path, bool video, const QString &user, bool imported) {
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
        r.video = video;
        r.imported = imported;
        if (!user.isEmpty()) r.users << user;
        out.push_back(r);
    };
    {
        Lock lk(&m_mutex);
        for (const auto &l : m_layers) {
            const SourceType t = l->type != SourceType::None ? l->type : l->missingType;
            if (t == SourceType::Video || t == SourceType::Image) add(l->sourcePath, t == SourceType::Video, l->name, false);
            auto scan = [&](const IsfInstance *inst, const QString &user) {
                if (!inst) return;
                for (const IsfInput &in : inst->inputs())
                    if (in.type == IsfInput::Image && !in.isInputImage && !in.imagePath.isEmpty())
                        add(in.imagePath, isVideoFile(in.imagePath), user, false);
            };
            scan(l->generator.get(), l->name);
            for (const auto &fx : l->effects) scan(fx.get(), l->name + QStringLiteral(" › ") + fx->name());
        }
        for (const QString &p : m_binItems) add(p, isVideoFile(p), QString(), true);
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
        bool uses = l.sourcePath == path && (l.type == SourceType::Video || l.type == SourceType::Image || l.missingType != SourceType::None);
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
    std::vector<std::pair<IsfInstance *, int>> inputs;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        const SourceType t = l->type != SourceType::None ? l->type : l->missingType;
        if (l->sourcePath == from && (t == SourceType::Video || t == SourceType::Image)) kind = t;
        mapping = l->mapping;
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
        // Le nouveau fichier peut être d'un autre type (vidéo remplacée par une image, ou l'inverse)
        const bool video = isImageFile(to) ? false : (isVideoFile(to) ? true : kind == SourceType::Video);
        ok = video ? setLayerVideo(i, to, err) : setLayerImage(i, to, err);
        if (ok) {
            Lock lk(&m_mutex);
            if (Layer *l = layer(i)) {
                const unsigned rev = l->mapping.revision;
                l->mapping = mapping; // le mapping calé reste intact
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
// Publication
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
            setPublishState(k, {PublishState::Off, QStringLiteral("Désactivé"), -1});
            continue;
        }
        if (!publishCompiledIn(k)) {
            setPublishState(k, {PublishState::Unavailable,
                                k == PublishKind::Syphon ? QStringLiteral("Syphon n'existe que sur macOS")
                                                         : QStringLiteral("Spout n'existe que sous Windows"),
                                -1});
            continue;
        }
        QString err;
        if (isGpuKind(k)) {
            auto p = k == PublishKind::Syphon ? createSyphonPublisher() : createSpoutPublisher();
            if (p && p->start(want.name, &err)) {
                m_gpuPubs[ki] = std::move(p);
                setPublishState(k, {PublishState::Ok, QStringLiteral("Actif : « %1 »").arg(want.name), -1});
            } else {
                setPublishState(k, {PublishState::Error, err.isEmpty() ? QStringLiteral("Démarrage impossible") : err, -1});
            }
        } else {
            std::shared_ptr<CpuPublisher> p(k == PublishKind::Ndi ? createNdiPublisher() : createOmtPublisher());
            if (p->start(want.name, s, &err)) {
                m_cpuPubs[ki] = p;
                cpuChanged = true;
                setPublishState(k, {PublishState::Ok, QStringLiteral("Actif : « %1 »").arg(want.name), 0});
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

    // Nombre de récepteurs, environ deux fois par seconde
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

    // 1) Image courante retournée (lignes de haut en bas) puis relue de façon asynchrone dans un PBO
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

    // 2) Image précédente, déjà disponible : copiée vers le fil d'envoi
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
                // Cadence annoncée : ne change que si la nouvelle valeur se confirme pendant 2 s
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
