#include "Engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QDebug>

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

int Layer::sourceWidth() const
{
    switch (type) {
    case SourceType::Video:
    case SourceType::Image: return sourceTex.w > 0 ? sourceTex.w : (video ? video->width() : 0);
    case SourceType::Isf: return genWidth;
    default: return 0;
    }
}

int Layer::sourceHeight() const
{
    switch (type) {
    case SourceType::Video:
    case SourceType::Image: return sourceTex.h > 0 ? sourceTex.h : (video ? video->height() : 0);
    case SourceType::Isf: return genHeight;
    default: return 0;
    }
}

// Rend le contexte du moteur courant le temps d'un bloc.
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
}

void Engine::doneCurrent()
{
    if (m_context) m_context->doneCurrent();
}

bool Engine::initialize(QString *err)
{
    m_context = new QOpenGLContext(this);
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
    m_blitProgram = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
        "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
        "void main(){ o = texture(u_tex, v_uv); }\n",
        &log);
    m_compProgram = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; out vec2 v_uv;\n"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; uniform float u_opacity; in vec2 v_uv; out vec4 o;\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); float a = c.a*u_opacity; o = vec4(c.rgb*a, a); }\n",
        &log);
    if (!m_blitProgram || !m_compProgram) {
        if (err) *err = QStringLiteral("Shaders internes : ") + log;
        return false;
    }
    m_blitTexLoc = f->glGetUniformLocation(m_blitProgram, "u_tex");
    m_compTexLoc = f->glGetUniformLocation(m_compProgram, "u_tex");
    m_compOpacityLoc = f->glGetUniformLocation(m_compProgram, "u_opacity");

    const unsigned char black[4] = {0, 0, 0, 0};
    f->glGenTextures(1, &m_blackTex);
    f->glBindTexture(GL_TEXTURE_2D, m_blackTex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    m_output.ensure(m_compSize.width(), m_compSize.height());
    m_context->doneCurrent();

    m_library.scan();
    m_clock.start();
    m_lastNs = m_clock.nsecsElapsed();
    m_initialized = true;
    return true;
}

void Engine::shutdown()
{
    if (!m_initialized) return;
    {
        ScopedCurrent sc(this);
        for (auto &l : m_layers) releaseLayer(*l);
        m_layers.clear();
        auto f = gl();
        m_output.destroy();
        if (m_blitProgram) f->glDeleteProgram(m_blitProgram);
        if (m_compProgram) f->glDeleteProgram(m_compProgram);
        GLuint bufs[] = {m_quadVbo, m_meshVbo, m_meshIbo};
        f->glDeleteBuffers(3, bufs);
        GLuint vaos[] = {m_quadVao, m_meshVao};
        f->glDeleteVertexArrays(2, vaos);
        f->glDeleteTextures(1, &m_blackTex);
    }
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Calques
// ---------------------------------------------------------------------------

void Engine::setCompositionSize(QSize s)
{
    s = s.expandedTo(QSize(16, 16)).boundedTo(QSize(16384, 16384));
    if (s == m_compSize) return;
    m_compSize = s;
    emit compositionSizeChanged(s);
}

int Engine::indexOf(const Layer *l) const
{
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].get() == l) return int(i);
    return -1;
}

int Engine::addLayer(const QString &name, int at)
{
    auto l = std::make_unique<Layer>();
    l->name = name.isEmpty() ? QStringLiteral("Calque %1").arg(layerCount() + 1) : name;
    l->genWidth = m_compSize.width();
    l->genHeight = m_compSize.height();
    at = std::clamp(at, 0, layerCount());
    m_layers.insert(m_layers.begin() + at, std::move(l));
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
    Layer *l = layer(i);
    if (!l) return;
    {
        ScopedCurrent sc(this);
        releaseLayer(*l);
    }
    m_layers.erase(m_layers.begin() + i);
    emit layersChanged();
}

void Engine::moveLayer(int from, int to)
{
    if (!layer(from)) return;
    to = std::clamp(to, 0, layerCount() - 1);
    if (from == to) return;
    auto l = std::move(m_layers[size_t(from)]);
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, std::move(l));
    emit layersChanged();
}

int Engine::duplicateLayer(int i)
{
    Layer *l = layer(i);
    if (!l) return -1;
    QJsonObject o = layerToJson(*l, QString());
    o["name"] = l->name + QStringLiteral(" copie");
    int ni = addLayer(QString(), i);
    layerFromJson(ni, o, QString(), nullptr);
    emit layersChanged();
    return ni;
}

void Engine::clearLayerSource(int i)
{
    Layer *l = layer(i);
    if (!l) return;
    ScopedCurrent sc(this);
    if (l->video) l->video->close();
    l->video.reset();
    l->sourceTex.destroy();
    if (l->generator) l->generator->releaseGl();
    l->generator.reset();
    l->generatorTarget.destroy();
    l->type = SourceType::None;
    l->sourcePath.clear();
    l->error.clear();
    l->finalTex = 0;
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

bool Engine::setLayerVideo(int i, const QString &path, QString *err)
{
    Layer *l = layer(i);
    if (!l) return false;
    auto dec = std::make_unique<VideoDecoder>();
    QString e;
    if (!dec->open(path, &e)) {
        if (err) *err = e;
        return false;
    }
    clearLayerSource(i);
    dec->setLoop(l->loop);
    l->video = std::move(dec);
    l->type = SourceType::Video;
    l->sourcePath = QFileInfo(path).absoluteFilePath();
    l->playhead = 0;
    l->playing = true;
    if (isDefaultMapping(l->mapping) && l->video->height() > 0)
        l->mapping.fitAspect(double(l->video->width()) / l->video->height(),
                             double(m_compSize.width()) / m_compSize.height());
    return true;
}

bool Engine::setLayerImage(int i, const QString &path, QString *err)
{
    Layer *l = layer(i);
    if (!l) return false;
    QImage img(path);
    if (img.isNull()) {
        if (err) *err = QStringLiteral("Image illisible : ") + path;
        return false;
    }
    clearLayerSource(i);
    img = img.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
    {
        ScopedCurrent sc(this);
        l->sourceTex.upload(img.constBits(), img.width(), img.height());
    }
    l->type = SourceType::Image;
    l->sourcePath = QFileInfo(path).absoluteFilePath();
    if (isDefaultMapping(l->mapping))
        l->mapping.fitAspect(double(img.width()) / img.height(), double(m_compSize.width()) / m_compSize.height());
    return true;
}

bool Engine::setLayerIsf(int i, const QString &path, QString *err)
{
    Layer *l = layer(i);
    if (!l) return false;
    clearLayerSource(i);
    auto inst = std::make_unique<IsfInstance>();
    bool ok;
    {
        ScopedCurrent sc(this);
        ok = inst->load(path);
    }
    l->type = SourceType::Isf;
    l->sourcePath = QFileInfo(path).absoluteFilePath();
    l->error = inst->error();
    l->generator = std::move(inst);
    if (!ok && err) *err = l->error;
    return ok;
}

void Engine::setGeneratorSize(int i, int w, int h)
{
    if (Layer *l = layer(i)) {
        l->genWidth = std::clamp(w, 1, 16384);
        l->genHeight = std::clamp(h, 1, 16384);
    }
}

void Engine::setLayerPlaying(int i, bool playing)
{
    Layer *l = layer(i);
    if (!l || !l->video) return;
    if (playing && !l->loop && l->duration() > 0 && l->playhead >= l->duration() - 1e-3) seekLayer(i, 0);
    l->playing = playing;
}

void Engine::setLayerLoop(int i, bool loop)
{
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
    Layer *l = layer(i);
    if (!l || !l->video) return;
    const double d = l->duration();
    if (d > 0) t = std::clamp(t, 0.0, d);
    l->playhead = t;
    l->video->seek(t);
}

int Engine::addEffect(int li, const QString &path, QString *err)
{
    Layer *l = layer(li);
    if (!l) return -1;
    auto inst = std::make_unique<IsfInstance>();
    {
        ScopedCurrent sc(this);
        if (!inst->load(path) && err) *err = inst->error();
    }
    l->effects.push_back(std::move(inst));
    return int(l->effects.size()) - 1;
}

void Engine::removeEffect(int li, int fx)
{
    Layer *l = layer(li);
    if (!l || fx < 0 || fx >= int(l->effects.size())) return;
    {
        ScopedCurrent sc(this);
        l->effects[size_t(fx)]->releaseGl();
    }
    l->effects.erase(l->effects.begin() + fx);
}

void Engine::moveEffect(int li, int from, int to)
{
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
    ScopedCurrent sc(this);
    return inst->setImageInput(input, path, err);
}

bool Engine::reloadIsf(IsfInstance *inst)
{
    if (!inst) return false;
    // On conserve les valeurs des paramètres à travers le rechargement.
    const QJsonObject saved = inst->save(QString());
    ScopedCurrent sc(this);
    bool ok = inst->load(inst->path());
    inst->restoreParams(saved.value("params").toObject(), QString());
    inst->enabled = saved.value("enabled").toBool(true);
    for (auto &l : m_layers)
        if (l->generator.get() == inst) l->error = inst->error();
    return ok;
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
    m_output.ensure(m_compSize.width(), m_compSize.height());
    m_output.clear(0, 0, 0, 1);
    f->glEnable(GL_BLEND);
    f->glUseProgram(m_compProgram);
    f->glUniform1i(m_compTexLoc, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindVertexArray(m_meshVao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_meshVbo);

    // Le calque d'index 0 est au-dessus : on dessine du dernier au premier.
    for (int i = layerCount() - 1; i >= 0; --i) {
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
    f->glDisable(GL_BLEND);
    f->glBindVertexArray(0);
}

void Engine::renderFrame()
{
    if (!m_initialized) return;
    const qint64 now = m_clock.nsecsElapsed();
    double dt = (now - m_lastNs) / 1e9;
    m_lastNs = now;
    dt = std::clamp(dt, 0.0, 0.25);
    if (dt > 0) m_fps = m_fps * 0.95 + (1.0 / dt) * 0.05;

    ScopedCurrent sc(this);
    IsfRenderContext rc;
    rc.dt = dt;
    rc.blackTex = m_blackTex;
    rc.drawQuad = [this] { drawQuad(); };
    rc.blit = [this](GLuint t, const RenderTarget &rt) { blit(t, rt); };

    for (auto &l : m_layers) updateSource(*l, dt);
    for (auto &l : m_layers) renderLayer(*l, rc);
    composite();
    gl()->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Les fenêtres d'affichage lisent la texture depuis d'autres contextes : on synchronise.
    gl()->glFinish();
}

QImage Engine::grabOutput()
{
    ScopedCurrent sc(this);
    if (!m_output.fbo) return {};
    QImage img(m_output.w, m_output.h, QImage::Format_RGBA8888);
    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_output.fbo);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glReadPixels(0, 0, m_output.w, m_output.h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return img.mirrored(false, true);
}

// ---------------------------------------------------------------------------
// Projet (JSON)
// ---------------------------------------------------------------------------

void Engine::newProject()
{
    {
        ScopedCurrent sc(this);
        for (auto &l : m_layers) releaseLayer(*l);
    }
    m_layers.clear();
    m_projectPath.clear();
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
    default: src["type"] = "none"; break;
    }
    if (l.type != SourceType::None) {
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
    Layer *l = layer(index);
    if (!l) return;
    l->name = o.value("name").toString(l->name);
    l->visible = o.value("visible").toBool(true);
    l->opacity = float(o.value("opacity").toDouble(1.0));
    l->blend = blendModeFromKey(o.value("blend").toString());

    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const QString path = type != "none" ? resolvePath(src, projectDir) : QString();
    QString err;
    if (type == "video") {
        l->loop = src.value("loop").toBool(true);
        l->speed = src.value("speed").toDouble(1.0);
        if (!setLayerVideo(index, path, &err) && warnings) *warnings << l->name + " : " + err;
        l->playing = src.value("playing").toBool(true);
    } else if (type == "image") {
        if (!setLayerImage(index, path, &err) && warnings) *warnings << l->name + " : " + err;
    } else if (type == "isf") {
        l->genWidth = src.value("width").toInt(m_compSize.width());
        l->genHeight = src.value("height").toInt(m_compSize.height());
        if (!setLayerIsf(index, path, &err) && warnings) *warnings << l->name + " : " + err;
        if (l->generator) {
            ScopedCurrent sc(this);
            l->generator->restoreParams(src.value("params").toObject(), projectDir);
        }
    }

    for (const QJsonValue &v : o.value("effects").toArray()) {
        const QJsonObject e = v.toObject();
        const QString p = resolvePath(e, projectDir);
        int fi = addEffect(index, p, &err);
        if (fi < 0) continue;
        IsfInstance *inst = l->effects[size_t(fi)].get();
        if (!inst->isValid() && warnings) *warnings << l->name + " / " + QFileInfo(p).fileName() + " : " + inst->error();
        inst->enabled = e.value("enabled").toBool(true);
        ScopedCurrent sc(this);
        inst->restoreParams(e.value("params").toObject(), projectDir);
    }
    // Le mapping est restauré après la source (qui ajuste sinon le ratio automatiquement).
    l->mapping.fromJson(o.value("mapping").toObject());
}

bool Engine::saveProject(const QString &path, const QJsonObject &uiState, QString *err)
{
    const QString dir = QFileInfo(path).absolutePath();
    QJsonObject root;
    root["app"] = "Lanterne";
    root["formatVersion"] = 1;
    root["composition"] = QJsonObject{{"width", m_compSize.width()}, {"height", m_compSize.height()}};
    QJsonArray layers;
    for (const auto &l : m_layers) layers.append(layerToJson(*l, dir));
    root["layers"] = layers;
    root["ui"] = uiState;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    m_projectPath = QFileInfo(path).absoluteFilePath();
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
    if (uiState) *uiState = root.value("ui").toObject();
    m_projectPath = QFileInfo(path).absoluteFilePath();
    emit layersChanged();
    if (!warnings.isEmpty() && err) *err = warnings.join('\n');
    return true;
}
