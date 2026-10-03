// Engine: one frame — the sources are updated, every layer is rendered in dependency order, the
// composition is assembled, then read back for the interface preview and for publishing.
#include "EngineInternal.h"

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
        if (l.isGroup) {
            // Children of a composition or a group, in list order (the list is normalized, so that is draw order)
            std::vector<Layer *> members;
            for (size_t k = 0; k < n; ++k)
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
    const QSize size = g.isComposition ? g.compSize() : m_compSize;
    g.groupTarget.ensure(size.width(), size.height());
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

// Each composition carries its picture to its own output: the master level and the blackout are applied
// there, into a double buffer the interface and the publishers read from.
void Engine::composite()
{
    auto f = gl();
    const double master = m_masterLevel.load() * m_blackLevel.load();
    for (auto &lp : m_layers) {
        Layer &v = *lp;
        if (!v.isComposition) continue;
        const QSize size = v.compSize();
        RenderTarget &out = v.compOut[v.compBack];
        out.ensure(size.width(), size.height());
        out.clear(0, 0, 0, 1);
        if (v.visible && v.finalTex) {
            out.bind();
            f->glDisable(GL_BLEND);
            blit(v.finalTex, out);
        }
        // Master and blackout: multiply the whole image by the level (constant blend color).
        if (master < 0.999) {
            out.bind();
            f->glEnable(GL_BLEND);
            const float m = float(master);
            f->glBlendColor(m, m, m, 1.0f);
            f->glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR);
            f->glUseProgram(m_blitProgram);
            f->glActiveTexture(GL_TEXTURE0);
            f->glBindTexture(GL_TEXTURE_2D, m_blackTex);
            f->glUniform1i(m_blitTexLoc, 0);
            drawQuad();
        }
        f->glDisable(GL_BLEND);
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
    renderPass(rc);
    composite();
    readSourcePreview();
    const bool publishChanged = m_publishDirty || m_tapDirty;
    m_mutex.unlock();

    if (publishChanged || !m_publishInit) applyPublishing();
    {
        // Publishing and the interface preview show the main composition
        Lock lk(&m_mutex);
        for (auto &l : m_layers)
            if (l->isComposition) {
                publishFrame(l->compOut[l->compBack]);
                break;
            }
    }

    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The preview reads the image from another context: wait for rendering to finish before publishing it.
    f->glFinish();
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers)
            if (l->isComposition) {
                l->compPublished = l->compBack;
                l->compBack = 1 - l->compBack;
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
    if (!m_context->makeCurrent(m_surface)) return;
    m_currentSurface = m_surface;
    frame(nextDt());
    // Manual mode: the compositions that have a window are presented in turn
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
    }
    if (m_currentSurface != m_surface) {
        m_context->makeCurrent(m_surface);
        m_currentSurface = m_surface;
    }
    if (!m_framePending.exchange(true)) emit frameRendered();
}

QImage Engine::grabOutput()
{
    QImage img;
    runGl([this, &img] {
        Lock lk(&m_mutex);
        const RenderTarget *main = nullptr;
        for (auto &l : m_layers)
            if (l->isComposition) {
                main = &l->compOut[l->compPublished.load()];
                break;
            }
        if (!main || !main->fbo) return;
        const RenderTarget &o = *main;
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
