// Engine: one frame — the sources are updated, every layer is rendered in dependency order, the
// composition is assembled, then read back for the interface preview and for publishing.
#include "EngineInternal.h"

#include <map>

#include <QImage>
#include <QOffscreenSurface>
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

// One frame of every layer, in dependency order: a layer whose source is another layer is rendered after it,
// and a group after its members. A layer used as a source is rendered even when it is hidden.
// A reference cycle (A on B, B on A) is not broken by an error: the layer reached a second time keeps the
// texture of the previous frame, which gives a one-frame feedback loop.
void Engine::renderPass(const IsfRenderContext &rc)
{
    const size_t n = m_layers.size();
    for (auto &l : m_layers) l->referenced = false;
    for (auto &l : m_layers) {
        if (l->type != SourceType::Layer || !l->sourceLayer) continue;
        const int si = indexOfId(l->sourceLayer);
        if (si >= 0) m_layers[size_t(si)]->referenced = true;
    }
    enum { Todo = 0, Doing = 1, Done = 2 };
    m_renderMark.assign(n, Todo);
    std::function<void(size_t)> render = [&](size_t i) {
        if (m_renderMark[i] != Todo) return; // done, or being rendered (cycle)
        m_renderMark[i] = Doing;
        Layer &l = *m_layers[i];
        if (l.type == SourceType::Layer && l.sourceLayer) {
            const int si = indexOfId(l.sourceLayer);
            if (si >= 0) render(size_t(si));
        }
        if (l.isViewport) {
            // What it sees: the items at the top of the list it shows, all rendered by now
            std::vector<Layer *> shown;
            for (size_t k = 0; k < n; ++k) {
                Layer &o = *m_layers[k];
                if (o.isViewport || o.parent || !o.shownIn(l.id)) continue;
                render(k);
                shown.push_back(&o);
            }
            renderViewport(l, shown, rc);
        } else if (l.isGroup) {
            // Its direct contents, in list order (groups inside are rendered first, by the recursion)
            std::vector<Layer *> members;
            for (size_t k = i + 1; k < n; ++k)
                if (m_layers[k]->parent == l.id) {
                    render(k);
                    members.push_back(m_layers[k].get());
                }
            if (l.visible || l.referenced) renderGroup(l, members, rc);
            else l.finalTex = l.rawTex = l.preFxTex = 0;
        } else {
            renderLayer(l, rc);
        }
        m_renderMark[i] = Done;
    };
    for (size_t i = 0; i < n; ++i) render(i);
}

void Engine::renderLayer(Layer &l, const IsfRenderContext &rc)
{
    l.finalTex = 0;
    l.rawTex = 0;
    l.preFxTex = 0;
    if (l.isGroup) return;  // rendered from its members (renderGroup)
    if (l.ended) return; // Stop mode, after the end: nothing
    GLuint tex = 0;
    int w = 0, h = 0;
    switch (l.type) {
    case SourceType::Layer:
        // Picture of another layer, already rendered this frame (the render pass follows the references).
        if (const Layer *s = l.sourceLayer ? layer(indexOfId(l.sourceLayer)) : nullptr) {
            tex = l.sourceTap == LayerTap::PreFx ? s->preFxTex : s->finalTex;
            w = l.sourceTap == LayerTap::PreFx ? s->preFxW : s->finalW;
            h = l.sourceTap == LayerTap::PreFx ? s->preFxH : s->finalH;
            l.srcWidth = w; // what the inspector and the ROI editor show
            l.srcHeight = h;
        }
        break;
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

// Source picture -> roi and color (when needed) -> effect chain -> l.finalTex
void Engine::processLayer(Layer &l, GLuint tex, int w, int h, bool premultiplied, const IsfRenderContext &rc)
{
    l.rawTex = tex;
    l.rawW = w;
    l.rawH = h;
    QRectF c = l.roi.normalized() & Layer::fullRoi();
    if (c.width() < 1e-4 || c.height() < 1e-4) c = Layer::fullRoi();
    const bool restricted = c != Layer::fullRoi();
    const ColorAdjust col = l.color.effective(); // switches honoured once, here
    if (restricted || !col.isIdentity() || premultiplied) {
        auto f = gl();
        const int cw = std::max(1, int(std::lround(w * c.width()))), ch = std::max(1, int(std::lround(h * c.height())));
        l.prepTarget.ensure(cw, ch);
        l.prepTarget.bind();
        f->glDisable(GL_BLEND);
        f->glUseProgram(m_prepProgram);
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, tex);
        f->glUniform1i(m_prepTexLoc, 0);
        // ROI: top-left origin in the UI, textures in OpenGL convention (origin bottom left)
        f->glUniform4f(m_prepRoiLoc, float(c.left()), float(1.0 - c.bottom()), float(c.right()), float(1.0 - c.top()));
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

    l.preFxTex = tex; // tap of a layer used as a source: after the ROI and the color, before the effects
    l.preFxW = w;
    l.preFxH = h;

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
// (premultiplied), then restricted, colored and processed by its effects like any layer.
void Engine::renderGroup(Layer &g, const std::vector<Layer *> &members, const IsfRenderContext &rc)
{
    g.finalTex = 0;
    g.rawTex = 0;
    g.groupTarget.ensure(m_compSize.width(), m_compSize.height());
    g.groupTarget.clear(0, 0, 0, 0);
    compositeLayers(g.groupTarget, members);
    processLayer(g, g.groupTarget.tex, g.groupTarget.w, g.groupTarget.h, true, rc);
}

// A viewport's picture: the part of the composition its Spatial places it on, drawn at its own size, then
// with its own ROI, color and effects like a group.
void Engine::renderViewport(Layer &v, const std::vector<Layer *> &shown, const IsfRenderContext &rc)
{
    v.finalTex = v.rawTex = v.preFxTex = 0;
    if (!v.visible) return;
    const QSize size = v.viewportSize();
    v.groupTarget.ensure(size.width(), size.height());
    v.groupTarget.clear(0, 0, 0, 0);
    compositeLayers(v.groupTarget, shown, v.mapping.bounds());
    processLayer(v, v.groupTarget.tex, v.groupTarget.w, v.groupTarget.h, true, rc);
}

// Draws the layers into the target; `view` is the part of the composition the target shows
// (normalized, origin top left: the whole composition by default)
void Engine::compositeLayers(const RenderTarget &target, const std::vector<Layer *> &layers, const QRectF &view)
{
    auto f = gl();
    target.bind();
    f->glEnable(GL_BLEND);
    f->glUseProgram(m_compProgram);
    f->glUniform1i(m_compTexLoc, 0);
    // Vertices are in the composition's clip space: scale and offset bring the view to the whole target
    const double rw = std::max(1e-6, view.width()), rh = std::max(1e-6, view.height());
    f->glUniform4f(m_compViewLoc, float(1.0 / rw), float(1.0 / rh), float((1.0 - 2.0 * view.left()) / rw - 1.0),
                   float(1.0 - (1.0 - 2.0 * view.top()) / rh));
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

// The whole composition, for the interface preview, then each viewport's output. The master level and the
// blackout are applied to both, into double buffers read by the interface, the windows and the publishers.
void Engine::composite()
{
    auto f = gl();
    const double master = m_masterLevel.load() * m_blackLevel.load();
    auto applyMaster = [&] {
        if (master >= 0.999) return;
        f->glEnable(GL_BLEND);
        const float m = float(master);
        f->glBlendColor(m, m, m, 1.0f);
        f->glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR);
        f->glUseProgram(m_blitProgram);
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, m_blackTex);
        f->glUniform1i(m_blitTexLoc, 0);
        drawQuad();
        f->glDisable(GL_BLEND);
    };

    RenderTarget &out = m_output[m_back];
    out.ensure(m_compSize.width(), m_compSize.height());
    out.clear(0, 0, 0, 1);
    std::vector<Layer *> top;
    for (auto &l : m_layers)
        if (!l->parent && !l->isViewport) top.push_back(l.get());
    compositeLayers(out, top);
    out.bind();
    applyMaster();

    static const Mapping kFull;
    for (auto &lp : m_layers) {
        Layer &v = *lp;
        if (!v.isViewport) continue;
        RenderTarget &o = v.vpOut[v.vpBack];
        const QSize size = v.viewportSize();
        o.ensure(size.width(), size.height());
        o.clear(0, 0, 0, 1);
        if (v.finalTex && v.opacity > 0.0f) {
            // The viewport's own picture fills its output, at its opacity
            o.bind();
            f->glEnable(GL_BLEND);
            f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            f->glUseProgram(m_compProgram);
            f->glUniform1i(m_compTexLoc, 0);
            f->glUniform4f(m_compViewLoc, 1, 1, 0, 0);
            f->glUniform1f(m_compOpacityLoc, v.opacity);
            f->glActiveTexture(GL_TEXTURE0);
            f->glBindVertexArray(m_meshVao);
            f->glBindBuffer(GL_ARRAY_BUFFER, m_meshVbo);
            kFull.buildVertices(kMeshSubdiv, m_meshScratch);
            f->glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(m_meshScratch.size() * sizeof(float)), m_meshScratch.data());
            f->glBindTexture(GL_TEXTURE_2D, v.finalTex);
            f->glDrawElements(GL_TRIANGLES, m_meshIndexCount, GL_UNSIGNED_INT, nullptr);
            f->glDisable(GL_BLEND);
            f->glBindVertexArray(0);
        }
        o.bind();
        applyMaster();
    }
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

    // Members of a hidden group are hidden (and silent) too, at any depth. A group comes before its contents.
    {
        std::map<quint64, bool> shown; // group id → visible, groups above included
        for (auto &l : m_layers) {
            const auto up = l->parent ? shown.find(l->parent) : shown.end();
            l->parentVisible = up == shown.end() || up->second;
            if (l->isGroup) shown[l->id] = l->parentVisible && l->visible;
        }
    }
    stepFade(m_realDt);
    for (auto &l : m_layers) updateSource(*l, dt);
    renderPass(rc);
    composite();
    readSourcePreview();
    // Publishing follows the viewports, however they changed (added, removed, undone, loaded, edited)
    bool publishChanged = m_publishDirty || m_tapDirty;
    if (!publishChanged) {
        size_t count = 0;
        for (const auto &l : m_layers) {
            if (!l->isViewport) continue;
            ++count;
            const auto it = m_pubs.find(l->id);
            if (it == m_pubs.end() || !(it->second->applied == l->vpPublish)) publishChanged = true;
        }
        publishChanged = publishChanged || count != m_pubs.size();
    }
    m_mutex.unlock();

    if (publishChanged) applyPublishing();
    {
        std::vector<std::pair<Publication *, const RenderTarget *>> outs;
        {
            Lock lk(&m_mutex);
            for (auto &l : m_layers) {
                if (!l->isViewport) continue;
                auto it = m_pubs.find(l->id);
                if (it != m_pubs.end()) outs.push_back({it->second.get(), &l->vpOut[l->vpBack]});
            }
        }
        for (auto &[pub, out] : outs) publishFrame(*pub, *out);
    }

    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The preview reads the image from another context: wait for rendering to finish before publishing it.
    f->glFinish();
    m_published = m_back;
    m_back = 1 - m_back;
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers)
            if (l->isViewport) {
                l->vpPublished = l->vpBack;
                l->vpBack = 1 - l->vpBack;
            }
    }
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
        f->glUseProgram(m_prepProgram); // unpremultiplied (groups), no roi, no color
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, l->rawTex);
        f->glUniform1i(m_prepTexLoc, 0);
        f->glUniform4f(m_prepRoiLoc, 0, 0, 1, 1);
        f->glUniform3f(m_prepAddLoc, 0, 0, 0);
        f->glUniform3f(m_prepRemoveLoc, 0, 0, 0);
        f->glUniform3f(m_prepBalanceLoc, 1, 1, 1);
        f->glUniform1i(m_prepUnpremulLoc, l->isGroup || l->isViewport ? 1 : 0);
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
    if (!m_context->makeCurrent(m_surface)) return;
    m_currentSurface = m_surface;
    frame(nextDt());
    presentViewports();
    if (!m_framePending.exchange(true)) emit frameRendered();
}

// Last finished picture of one viewport (what its window and its publishers get)
QImage Engine::grabViewport(quint64 viewport)
{
    QImage img;
    runGl([this, viewport, &img] {
        Lock lk(&m_mutex);
        for (auto &l : m_layers) {
            if (!l->isViewport || l->id != viewport) continue;
            const RenderTarget &o = l->vpOut[l->vpPublished.load()];
            if (!o.fbo) return;
            img = QImage(o.w, o.h, QImage::Format_RGBA8888);
            auto f = gl();
            f->glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
            f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
            f->glReadPixels(0, 0, o.w, o.h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
            f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    });
    return img.mirrored(false, true);
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
// Publishing (NDI, OMT, Syphon, Spout)
// ---------------------------------------------------------------------------
void Engine::setPublishSettings(quint64 viewport, const PublishSettings &s)
{
    Lock lk(&m_mutex);
    for (auto &l : m_layers)
        if (l->isViewport && l->id == viewport && l->vpPublish != s) {
            l->vpPublish = s;
            m_publishDirty = true;
        }
}

PublishSettings Engine::publishSettings(quint64 viewport) const
{
    Lock lk(&m_mutex);
    for (const auto &l : m_layers)
        if (l->isViewport && l->id == viewport) return l->vpPublish;
    return {};
}

void Engine::setTestTap(std::function<void(const CpuFrame &)> fn)
{
    Lock lk(&m_mutex);
    m_tap = std::move(fn);
    m_tapDirty = true;
    m_publishDirty = true;
}

PublishState Engine::publishState(quint64 viewport, PublishKind k) const
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    auto it = m_pubs.find(viewport);
    return it == m_pubs.end() ? PublishState() : it->second->states[int(k)];
}

void Engine::setPublishState(Publication &pub, PublishKind k, PublishState st)
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    pub.states[int(k)] = std::move(st);
}

static bool isGpuKind(PublishKind k) { return k == PublishKind::Syphon || k == PublishKind::Spout; }

// Every viewport's publishers follow its settings; those of a viewport that is gone are stopped.
void Engine::applyPublishing()
{
    std::vector<std::pair<quint64, PublishSettings>> want;
    std::function<void(const CpuFrame &)> tap;
    bool tapDirty;
    {
        Lock lk(&m_mutex);
        for (const auto &l : m_layers)
            if (l->isViewport) want.push_back({l->id, l->vpPublish});
        m_publishDirty = false;
        tap = m_tap;
        tapDirty = m_tapDirty;
        m_tapDirty = false;
    }
    for (auto it = m_pubs.begin(); it != m_pubs.end();) {
        const bool alive = std::any_of(want.begin(), want.end(), [&](const auto &w) { return w.first == it->first; });
        if (alive) {
            ++it;
            continue;
        }
        releasePublication(*it->second);
        std::lock_guard<std::mutex> lk(m_stateMutex);
        it = m_pubs.erase(it);
    }
    for (size_t k = 0; k < want.size(); ++k) {
        auto &slot = m_pubs[want[k].first];
        if (!slot) {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            slot = std::make_unique<Publication>();
        }
        Publication &pub = *slot;
        // The test tap follows the main viewport
        if (k == 0 && tapDirty) {
            pub.tap = tap ? std::shared_ptr<CpuPublisher>(createTapPublisher(tap)) : nullptr;
            pub.init = false; // the send thread is rebuilt below
        } else if (k != 0 && pub.tap) {
            pub.tap.reset();
            pub.init = false;
        }
        applyPublication(pub, want[k].second, k == 0);
    }
}

void Engine::applyPublication(Publication &pub, const PublishSettings &s, bool withTap)
{
    (void)withTap;
    const bool first = !pub.init;
    pub.init = true;
    bool cpuChanged = first;
    for (int ki = 0; ki < kPublishKindCount; ++ki) {
        const PublishKind k = PublishKind(ki);
        const PublishTarget &want = s.targets[ki];
        const PublishTarget &had = pub.applied.targets[ki];
        const bool optionsChanged = s.libraryFolder != pub.applied.libraryFolder
                                    || (k == PublishKind::Omt && s.omtQuality != pub.applied.omtQuality);
        if (!first && want == had && !(want.enabled && optionsChanged)) continue;

        if (isGpuKind(k)) {
            pub.gpu[ki].reset();
        } else if (pub.cpu[ki]) {
            pub.cpu[ki].reset();
            cpuChanged = true;
        }
        if (!want.enabled) {
            setPublishState(pub, k, {PublishState::Off, QStringLiteral("Disabled"), -1});
            continue;
        }
        if (!publishCompiledIn(k)) {
            setPublishState(pub, k,
                            {PublishState::Unavailable,
                             k == PublishKind::Syphon ? QStringLiteral("Syphon is only available on macOS")
                                                      : QStringLiteral("Spout is only available on Windows"),
                             -1});
            continue;
        }
        QString err;
        if (isGpuKind(k)) {
            auto p = k == PublishKind::Syphon ? createSyphonPublisher() : createSpoutPublisher();
            if (p && p->start(want.name, &err)) {
                pub.gpu[ki] = std::move(p);
                setPublishState(pub, k, {PublishState::Ok, QStringLiteral("Active: \"%1\"").arg(want.name), -1});
            } else {
                setPublishState(pub, k, {PublishState::Error, err.isEmpty() ? QStringLiteral("Failed to start") : err, -1});
            }
        } else {
            std::shared_ptr<CpuPublisher> p(k == PublishKind::Ndi ? createNdiPublisher() : createOmtPublisher());
            if (p->start(want.name, s, &err)) {
                pub.cpu[ki] = p;
                cpuChanged = true;
                setPublishState(pub, k, {PublishState::Ok, QStringLiteral("Active: \"%1\"").arg(want.name), 0});
            } else {
                setPublishState(pub, k, {PublishState::Error, err, -1});
            }
        }
    }
    pub.applied = s;
    if (cpuChanged) {
        std::vector<std::shared_ptr<CpuPublisher>> pubs;
        for (auto &p : pub.cpu)
            if (p) pubs.push_back(p);
        if (pub.tap) pubs.push_back(pub.tap);
        if (!pubs.empty() && !pub.sender) pub.sender = std::make_unique<CpuSendThread>();
        if (pub.sender) pub.sender->setPublishers(std::move(pubs));
        pub.pboPending = false;
    }
}

// GPU publishers need the context, the send thread is stopped before the CPU publishers go
void Engine::releasePublication(Publication &pub)
{
    for (auto &p : pub.gpu) p.reset();
    if (pub.sender) pub.sender->setPublishers({});
    pub.sender.reset();
    for (auto &p : pub.cpu) p.reset();
    pub.tap.reset();
    pub.readback.destroy();
    if (pub.pbo[0]) gl()->glDeleteBuffers(2, pub.pbo);
    pub.pbo[0] = pub.pbo[1] = 0;
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

void Engine::publishFrame(Publication &pub, const RenderTarget &out)
{
    for (int ki : {int(PublishKind::Syphon), int(PublishKind::Spout)})
        if (pub.gpu[ki]) pub.gpu[ki]->publish(out.tex, out.w, out.h);

    // Receiver count, about twice per second
    if ((m_frameCount.load() % 30) == 0) {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        for (int ki = 0; ki < kPublishKindCount; ++ki) {
            if (pub.states[ki].level != PublishState::Ok) continue;
            if (pub.cpu[ki]) pub.states[ki].receivers = pub.cpu[ki]->receivers();
            else if (pub.gpu[ki]) pub.states[ki].receivers = pub.gpu[ki]->receivers();
        }
    }

    if (!pub.sender || !pub.sender->hasPublishers() || !out.tex) {
        pub.pboPending = false;
        return;
    }
    auto f = gl();
    const int w = out.w, h = out.h;
    const GLsizeiptr bytes = GLsizeiptr(w) * h * 4;
    if (!pub.pbo[0] || w != pub.pboW || h != pub.pboH) {
        if (!pub.pbo[0]) f->glGenBuffers(2, pub.pbo);
        for (GLuint b : pub.pbo) {
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
            f->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        pub.pboW = w;
        pub.pboH = h;
        pub.pboPending = false;
    }
    pub.readback.ensure(w, h);

    // 1) Current frame flipped (top-to-bottom rows), then read back asynchronously into a PBO
    pub.readback.bind();
    f->glDisable(GL_BLEND);
    f->glUseProgram(m_flipProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, out.tex);
    f->glUniform1i(m_flipTexLoc, 0);
    drawQuad();
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, pub.pbo[pub.pboIndex]);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

    // 2) Previous frame, already available: copied to the send thread
    if (pub.pboPending) {
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, pub.pbo[1 - pub.pboIndex]);
        const void *ptr = f->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (ptr) {
            if (CpuFrame *cf = pub.sender->acquire()) {
                cf->bgra.resize(size_t(bytes));
                std::memcpy(cf->bgra.data(), ptr, size_t(bytes));
                cf->width = w;
                cf->height = h;
                cf->stride = w * 4;
                cf->timestamp100ns = qint64(pub.pboTime);
                // Announced frame rate: only changes if the new value holds for 2 s
                const int rate = standardRate(m_fps.load());
                const double nowS = m_clock.nsecsElapsed() / 1e9;
                if (pub.announcedRate == 0) pub.announcedRate = rate;
                if (rate == pub.announcedRate) pub.rateSince = nowS;
                else if (nowS - pub.rateSince > 2.0) pub.announcedRate = rate;
                cf->fpsN = pub.announcedRate;
                cf->fpsD = 1;
                pub.sender->submit(cf);
            }
            f->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    pub.pboTime = double(m_clock.nsecsElapsed() / 100);
    pub.pboPending = true;
    pub.pboIndex = 1 - pub.pboIndex;
}
