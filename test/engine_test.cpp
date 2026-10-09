// Headless engine tests: load/save, video, ISF (multi-pass, .vs, errors).
#include "Commands.h"
#include "Engine.h"
#include "LayerTree.h"
#include "Osc.h"
#include "Zeroconf.h"
#include <QJsonArray>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>
#include <QColor>
#include <QElapsedTimer>
#include <QLineF>
#include <mutex>
#include <QRegularExpression>
#include <QUndoStack>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QJsonDocument>
#include <QLineF>
#include <QSurfaceFormat>
#include <QThread>
#include <cmath>
#include <atomic>
#include <functional>
#include <cstdio>
#include <cstring>

static int failures = 0;
int runVideoTests(Engine &e, const QString &root, const QString &tmp); // video_test.cpp
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
        else std::printf("ok    %s\n", #cond);                                      \
    } while (0)

static QJsonObject readJson(const QString &p)
{
    QFile f(p);
    f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(f.readAll()).object();
}

int main(int argc, char **argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    const QString root = QString(TEST_DIR);
    const QString tmp = QDir::tempPath() + "/fulskrin_test";
    QDir().mkpath(tmp);

    Engine e;
    QString err;
    CHECK(e.initialize(&err));
    // A composition is always shown through a viewport, and viewports come first in the list:
    // the layers of these tests start at row V.
    const int V = 1;

    // Compatibility mode: fulskrin_tests --check-isf <folder>  -> compiles and renders every shader in the folder
    if (argc >= 3 && QString(argv[1]) == "--check-isf") {
        QDirIterator it(argv[2], {"*.fs"}, QDir::Files, QDirIterator::Subdirectories);
        int total = 0, ok = 0, skipped = 0;
        int v = e.addLayer("src");
        e.setLayerVideo(v, root + "/media/h264.mp4", &err);
        int g = e.addLayer("gen");
        while (it.hasNext()) {
            const QString path = it.next();
            IsfInstance::Header h = IsfInstance::readHeader(path);
            if (!h.ok) { std::printf("HEADER   %s\n", qPrintable(path)); ++total; continue; }
            if (h.isTransition) { ++skipped; continue; }
            ++total;
            QString msg;
            bool valid;
            if (h.isFilter) {
                int fx = e.addEffect(v, path, &msg);
                valid = e.layer(v)->effects[size_t(fx)]->isValid();
                for (int i = 0; i < 2; ++i) e.renderFrame();
                e.removeEffect(v, fx);
            } else {
                valid = e.setLayerIsf(g, path, &msg);
                for (int i = 0; i < 2; ++i) e.renderFrame();
            }
            if (valid) ++ok;
            else std::printf("FAIL     %s\n%s\n", qPrintable(QFileInfo(path).fileName()), qPrintable(msg.left(600)));
        }
        std::printf("\nISF: %d/%d compiled (%d transitions skipped)\n", ok, total, skipped);
        e.shutdown();
        return ok == total ? 0 : 1;
    }

    // 0. Video pipeline: pixel formats, HAP, upload buffers (video_test.cpp). Leaves a new project.
    failures += runVideoTests(e, root, tmp);

    // 1. JSON round trip (project built here: generator + warped mesh, video + effects)
    const QString isf = root + "/../isf";
    int gen = e.addLayer("Test Pattern");
    CHECK(e.setLayerIsf(gen, isf + "/generators/TestPattern.fs", &err));
    e.layer(gen)->mapping.meshMode = true;
    e.layer(gen)->mapping.setControlPoint(1, 1, QPointF(0.4, 0.3));
    e.layer(gen)->mapping.setCorner(1, QPointF(0.9, 0.1));
    int vid = e.addLayer("Video", V + 1);
    CHECK(e.setLayerVideo(vid, root + "/media/h264.mp4", &err));
    e.addEffect(vid, isf + "/effects/ColorCorrection.fs", &err);
    e.addEffect(vid, isf + "/effects/Trails.fs", &err);
    e.layer(vid)->effects[0]->inputs()[1].fValue = 0.5;
    e.layer(vid)->blend = BlendMode::Screen;
    CHECK(e.layerCount() == V + 2);
    for (int i = 0; i < 10; ++i) e.renderFrame();
    const QPointF cp = e.layer(gen)->mapping.controlPoint(1, 1);
    CHECK(e.saveProject(tmp + "/a.fulskrin", {}, &err));
    CHECK(e.loadProject(tmp + "/a.fulskrin", nullptr, &err));
    CHECK(err.isEmpty());
    CHECK(e.saveProject(tmp + "/b.fulskrin", {}, &err));
    QJsonObject a = readJson(tmp + "/a.fulskrin"), b = readJson(tmp + "/b.fulskrin");
    CHECK(a.value("layers") == b.value("layers"));
    CHECK(e.layer(V + 0)->mapping.meshMode);
    CHECK(QLineF(e.layer(V + 0)->mapping.controlPoint(1, 1), cp).length() < 1e-9);
    CHECK(e.layer(V + 1)->blend == BlendMode::Screen);
    CHECK(e.layer(V + 1)->effects.size() == 2 && std::abs(e.layer(V + 1)->effects[0]->inputs()[1].fValue - 0.5) < 1e-9);

    { // Position / scale of the mapped shape: an exact affine transform of every point (corners and mesh warp)
        Mapping m;
        m.corners[0] = {0.1, 0.2};
        m.corners[1] = {0.7, 0.15};
        m.corners[2] = {0.8, 0.9};
        m.corners[3] = {0.05, 0.8};
        m.setControlPoint(1, 1, m.controlPoint(1, 1) + QPointF(0.04, -0.03));
        const QRectF from = m.bounds();
        const QPointF p = m.map(0.37, 0.61);
        const QRectF to(0.3, 0.25, from.width() * 0.5, from.height() * 1.5);
        m.setBounds(to);
        const QPointF expected(to.left() + (p.x() - from.left()) * 0.5, to.top() + (p.y() - from.top()) * 1.5);
        CHECK(QLineF(m.map(0.37, 0.61), expected).length() < 1e-9);
        CHECK(std::abs(m.bounds().width() - to.width()) < 1e-9 && std::abs(m.bounds().center().x() - to.center().x()) < 1e-9);
    }

    // 2. Video: seeking and end of playback without loop
    e.newProject();
    int v = e.addLayer("v");
    CHECK(e.setLayerVideo(v, root + "/media/h264.mp4", &err));
    CHECK(std::abs(e.layer(v)->duration() - 4.0) < 0.1);
    CHECK(e.layer(v)->video->width() == 1280);
    e.setLayerPlayMode(v, PlayMode::OneShot);
    e.seekLayer(v, 3.9);
    { // until the end of the media is reached and a frame uploaded (slow machines: up to 5 s)
        QElapsedTimer t;
        t.start();
        do {
            e.renderFrame();
            QThread::msleep(10);
        } while ((e.layer(v)->playing || !e.layer(v)->videoTex || e.layer(v)->videoTex->width() != 1280) && t.elapsed() < 5000);
    }
    CHECK(!e.layer(v)->playing);           // stops at end of media
    CHECK(e.layer(v)->videoTex && e.layer(v)->videoTex->width() == 1280); // a frame was indeed uploaded to the GPU
    e.setLayerPlaying(v, true);             // restarts from the beginning
    CHECK(e.layer(v)->position() < 0.01);

    // 3. ISF: custom vertex shader, broken shader, passes with computed size
    int fx = e.addEffect(v, root + "/isf/Offset.fs", &err);
    CHECK(e.layer(v)->effects[size_t(fx)]->isValid());
    QFile bad(tmp + "/Broken.fs");
    bad.open(QIODevice::WriteOnly);
    bad.write("/*{ \"INPUTS\": [ {\"NAME\":\"inputImage\",\"TYPE\":\"image\"}, ] }*/\nvoid main(){ gl_FragColor = undefinedThing; }\n");
    bad.close();
    int fx2 = e.addEffect(v, tmp + "/Broken.fs", &err);
    CHECK(!e.layer(v)->effects[size_t(fx2)]->isValid());
    CHECK(e.layer(v)->effects[size_t(fx2)]->error().contains("undefinedThing"));
    int fx3 = e.addEffect(v, root + "/../isf/effects/Blur.fs", &err);
    CHECK(e.layer(v)->effects[size_t(fx3)]->isValid());
    for (int i = 0; i < 5; ++i) e.renderFrame(); // the broken shader passes the image through
    QImage out = e.grabOutput();
    CHECK(!out.isNull());

    // 4. Undo / redo (manual mode)
    {
        QUndoStack undo;
        e.newProject();
        int g = e.addLayer("Test Pattern");
        e.setLayerIsf(g, root + "/../isf/generators/TestPattern.fs", &err);
        undo.push(new cmd::AddLayer(&e, g, "add"));
        // Parameter: two close moves merge into a single step
        IsfValue v0 = cmd::resolveIsf(&e, g, -1)->inputs()[0].value(), v1 = v0, v2 = v0;
        v1.f = 20;
        v2.f = 30;
        undo.push(new cmd::SetParam(&e, g, -1, 0, v0, v1, "divisions"));
        undo.push(new cmd::SetParam(&e, g, -1, 0, v1, v2, "divisions"));
        CHECK(undo.count() == 2);
        CHECK(cmd::resolveIsf(&e, g, -1)->inputs()[0].fValue == 30);
        undo.undo();
        CHECK(cmd::resolveIsf(&e, g, -1)->inputs()[0].fValue == v0.f);
        undo.redo();
        // ISF speed: undoable, merged, saved and restored, exposed to timelines
        {
            CHECK(cmd::resolveIsf(&e, g, -1)->speed == 1.0);
            const int n0 = undo.count();
            undo.push(new cmd::SetIsfSpeed(&e, g, -1, 1.0, 2.0));
            undo.push(new cmd::SetIsfSpeed(&e, g, -1, 2.0, 3.0));
            CHECK(undo.count() == n0 + 1 && cmd::resolveIsf(&e, g, -1)->speed == 3.0);
            undo.undo();
            CHECK(cmd::resolveIsf(&e, g, -1)->speed == 1.0);
            undo.redo();
            bool found = false;
            for (const auto &p : e.animatableParams(e.layerId(g))) found |= (p.path == "source/speed");
            CHECK(found);
            // The timeline menu and the snapshot timing offer the speed of an effect too
            const int fxi = e.addEffect(g, QStringLiteral(TEST_DIR) + "/isf/Offset.fs", &err);
            CHECK(fxi >= 0);
            bool fxFound = false;
            QString fxPath;
            for (const auto &p : e.animatableParams(e.layerId(g)))
                if (p.path.startsWith("fx/") && p.path.endsWith("/speed")) fxFound = true, fxPath = p.path;
            CHECK(fxFound);
            CHECK(Engine::timingKey({"fx", "0", "speed"}, e.layerJson(g)) == fxPath);
            CHECK(Engine::timingKey({"source", "speed"}, e.layerJson(g)) == "source/speed");
            CHECK(e.layerJson(g).value("source").toObject().contains("speed"));
            CHECK(e.layerJson(g).value("fx").toArray().at(0).toObject().contains("speed"));
            e.removeEffect(g, fxi);
            CHECK(e.saveProject(tmp + "/speed.fulskrin", {}, &err));
            e.newProject();
            CHECK(e.loadProject(tmp + "/speed.fulskrin", nullptr, &err));
            CHECK(e.layerCount() == V + 1 && cmd::resolveIsf(&e, V + 0, -1)->speed == 3.0);
            e.newProject();
            g = e.addLayer("Test Pattern");
            e.setLayerIsf(g, root + "/../isf/generators/TestPattern.fs", &err);
            undo.clear();
            undo.push(new cmd::AddLayer(&e, g, "add"));
            v0 = cmd::resolveIsf(&e, g, -1)->inputs()[0].value();
            v1 = v0; v2 = v0; v1.f = 20; v2.f = 30;
            undo.push(new cmd::SetParam(&e, g, -1, 0, v0, v2, "divisions"));
        }
        // Mapping
        Mapping before = cmd::SetMapping::read(&e, g), after = before;
        after.setCorner(0, QPointF(0.2, 0.2));
        undo.push(new cmd::SetMapping(&e, g, before, after, "corner"));
        CHECK(e.layer(g)->mapping.corners[0] == QPointF(0.2, 0.2));
        undo.undo();
        CHECK(e.layer(g)->mapping.corners[0] == before.corners[0]);
        undo.redo();
        // Delete, then restore identically
        const QJsonObject snap = e.layerJson(g);
        undo.push(new cmd::RemoveLayer(&e, g));
        CHECK(e.layerCount() == V + 0);
        undo.undo();
        CHECK(e.layerCount() == V + 1);
        CHECK(e.layerJson(V + 0) == snap);
        // Effects
        const QJsonArray fxBefore = e.effectsJson(V + 0);
        e.addEffect(V + 0, root + "/../isf/effects/Hue.fs", &err);
        undo.push(new cmd::SetEffects(&e, V + 0, fxBefore, "fx"));
        CHECK(e.layer(V + 0)->effects.size() == 1);
        undo.undo();
        CHECK(e.layer(V + 0)->effects.empty());
        undo.redo();
        CHECK(e.layer(V + 0)->effects.size() == 1);
        // Back to the very start of the stack
        while (undo.canUndo()) undo.undo();
        CHECK(e.layerCount() == V + 0);
    }

    // 4b. Structure helpers (viewports, groups inside groups)
    {
        auto item = [](quint64 id, quint64 parent) { return TreeNode{id, parent, TreeNode::Item}; };
        auto group = [](quint64 id, quint64 parent) { return TreeNode{id, parent, TreeNode::Group}; };
        auto viewport = [](quint64 id) { return TreeNode{id, 0, TreeNode::Viewport}; };
        LayerTree t = {item(1, 0), group(2, 0), item(3, 2), viewport(9), item(4, 0), item(5, 2)};
        LayerTree n = tree::normalized(t);
        // Viewports first, then the contents of a group right after it
        CHECK(n.size() == 6 && n[0].id == 9 && n[1].id == 1 && n[2].id == 2 && n[3].id == 3 && n[4].id == 5 &&
              n[5].id == 4);
        LayerTree in = tree::intoGroup(n, {1, 4}, 2);
        CHECK(in[1].id == 2 && in.back().id == 4 && in.back().parent == 2 && in[4].id == 1 && in[4].parent == 2);
        LayerTree out = tree::moved(in, {3}, 0, 0); // to the end, top level
        CHECK(out.back().id == 3 && out.back().parent == 0);
        LayerTree un = tree::ungrouped(in, 2);
        CHECK(un.back().id == 2 && un[1].id == 3 && un[1].parent == 0);
        LayerTree g = tree::moved(n, {2}, 1, 0); // a group moves with its contents
        CHECK(g[1].id == 2 && g[2].id == 3 && g[3].id == 5 && g[4].id == 1);
        // A group inside a group, and its contents follow it at every depth
        LayerTree nest = tree::intoGroup(n, {2}, 0); // 0 is not a group: nothing moves
        CHECK(nest == n);
        LayerTree two = tree::normalized({viewport(9), group(10, 0), item(11, 10), group(2, 10), item(3, 2), item(4, 0)});
        CHECK(two[1].id == 10 && two[2].id == 11 && two[3].id == 2 && two[4].id == 3 && two[5].id == 4);
        CHECK(tree::depthOf(two, 3) == 2 && tree::depthOf(two, 2) == 1 && tree::depthOf(two, 4) == 0);
        CHECK(tree::descendants(two, 10) == std::vector<quint64>({11, 2, 3}));
        // Never inside itself (directly or through a group it holds), never inside a viewport
        CHECK(tree::intoGroup(two, {10}, 2) == two);
        LayerTree loop = {viewport(9), group(10, 2), group(2, 10)};
        const LayerTree cut = tree::normalized(loop);
        CHECK(cut[size_t(tree::indexOf(cut, 10))].parent == 0 || cut[size_t(tree::indexOf(cut, 2))].parent == 0);
        LayerTree inVp = tree::normalized({viewport(9), item(1, 9)});
        CHECK(inVp[1].parent == 0);
        // Ungrouping a group inside a group: its contents go up one level only
        LayerTree up = tree::ungrouped(two, 2);
        CHECK(up[size_t(tree::indexOf(up, 3))].parent == 10);
    }

    // 4c. Groups, roi, color, effects switch, lock, blackout (manual rendering)
    {
        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        QImage rb(64, 32, QImage::Format_RGB32); // left half red, right half blue
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) rb.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        rb.save(tmp + "/redblue.png");
        auto render = [&] {
            for (int k = 0; k < 2; ++k) e.renderFrame();
            return e.grabOutput();
        };
        auto px = [](const QImage &img, int x, int y) { return QColor(img.pixel(x, y)); };
        int l = e.addLayer("img");
        CHECK(e.setLayerImage(l, tmp + "/redblue.png", &err));
        QImage img = render();
        CHECK(px(img, 8, 16).red() > 250 && px(img, 56, 16).blue() > 250);
        // Roi: the right half only fills the layer
        e.layer(l)->roi = QRectF(0.5, 0, 0.5, 1);
        img = render();
        CHECK(px(img, 8, 16).blue() > 250 && px(img, 8, 16).red() < 5);
        e.layer(l)->roi = Layer::fullRoi();
        // Color: remove red, add green
        e.layer(l)->color.remove[0] = 1.0f;
        e.layer(l)->color.add[1] = 0.5f;
        img = render();
        CHECK(px(img, 8, 16).red() < 5 && std::abs(px(img, 8, 16).green() - 128) < 4);
        e.layer(l)->color = ColorAdjust();
        // Balance: warmer = more red, less blue; magenta tint = less green; luminance kept
        {
            QImage gray(64, 32, QImage::Format_RGB32);
            gray.fill(qRgb(128, 128, 128));
            gray.save(tmp + "/gray.png");
            const int gl = e.addLayer("gray", V + 0);
            e.setLayerImage(gl, tmp + "/gray.png", &err);
            e.layer(gl)->color.temp = 4000;
            QColor c = px(render(), 32, 16);
            CHECK(c.red() > 140 && c.blue() < 110 && std::abs(qGray(c.rgb()) - 128) < 12);
            e.layer(gl)->color.temp = 0;
            e.layer(gl)->color.tint = 100;
            c = px(render(), 32, 16);
            CHECK(c.green() < 120 && c.red() > 135 && c.blue() > 135);
            e.removeLayer(gl);
        }
        // Effects switch: FlipCrop flips the picture, the general switch bypasses the chain
        const int fx = e.addEffect(l, isf + "/effects/FlipCrop.fs", &err);
        CHECK(fx == 0);
        for (IsfInput &in : e.layer(l)->effects[0]->inputs())
            if (in.name == "flipH") in.bValue = true;
        img = render();
        CHECK(px(img, 8, 16).blue() > 250);
        e.layer(l)->effectsEnabled = false;
        img = render();
        CHECK(px(img, 8, 16).red() > 250);
        e.layer(l)->effectsEnabled = true;
        e.removeEffect(l, 0);

        // Group: the layer moves in, the group's opacity, visibility, roi and color apply to it
        int g = e.addGroup("G", V + 0);
        CHECK(e.layer(g)->isGroup && e.layerCount() == V + 2);
        const quint64 gid = e.layerId(g), lid = e.layerId(V + 1);
        e.setStructure(tree::intoGroup(e.structure(), {lid}, gid));
        CHECK(e.groupIndexOf(V + 1) == V + 0 && e.groupMembers(V + 0) == QList<int>{V + 1});
        img = render();
        CHECK(px(img, 8, 16).red() > 250 && px(img, 56, 16).blue() > 250); // a group with defaults changes nothing
        e.layer(V + 0)->opacity = 0.5f;
        img = render();
        CHECK(std::abs(px(img, 8, 16).red() - 128) < 6);
        e.layer(V + 0)->opacity = 1.0f;
        e.layer(V + 0)->roi = QRectF(0, 0, 0.5, 1); // left half of the composition: red everywhere
        img = render();
        CHECK(px(img, 56, 16).red() > 250 && px(img, 56, 16).blue() < 5);
        e.layer(V + 0)->roi = Layer::fullRoi();
        e.layer(V + 0)->color.remove[2] = 1.0f;
        img = render();
        CHECK(px(img, 56, 16).blue() < 5 && px(img, 8, 16).red() > 250);
        e.layer(V + 0)->color = ColorAdjust();
        e.layer(V + 0)->enabled = false;
        img = render();
        CHECK(qGray(img.pixel(8, 16)) < 3 && !e.layer(V + 1)->parentEnabled);
        e.layer(V + 0)->enabled = true;
        // Source preview: the group's composite, at the requested size
        e.requestSourcePreview(gid, 32);
        for (int k = 0; k < 8; ++k) e.renderFrame();
        quint64 pid = 0;
        const QImage prev = e.sourcePreview(&pid);
        CHECK(pid == gid && prev.width() == 32 && prev.height() == 16 && QColor(prev.pixel(4, 8)).red() > 250);
        e.requestSourcePreview(0);
        // Lock: of the layer itself or of its group
        CHECK(!e.isLocked(V + 1));
        e.layer(V + 0)->locked = true;
        CHECK(e.isLocked(V + 1) && e.isLocked(V + 0));
        e.layer(V + 0)->locked = false;

        // Save / load keeps the structure and the new properties
        e.layer(V + 1)->roi = QRectF(0.1, 0.2, 0.5, 0.6);
        e.layer(V + 1)->color.add[2] = 0.25f;
        e.layer(V + 1)->effectsEnabled = false;
        e.layer(V + 0)->locked = true;
        const LayerTree before = e.structure();
        CHECK(e.saveProject(tmp + "/groups.fulskrin", {}, &err));
        CHECK(e.loadProject(tmp + "/groups.fulskrin", nullptr, &err));
        CHECK(e.structure() == before);
        CHECK(e.layer(V + 0)->locked && e.layer(V + 0)->isGroup);
        CHECK(QLineF(e.layer(V + 1)->roi.topLeft(), QPointF(0.1, 0.2)).length() < 1e-9 && std::abs(e.layer(V + 1)->roi.width() - 0.5) < 1e-9);
        CHECK(std::abs(e.layer(V + 1)->color.add[2] - 0.25f) < 1e-6 && !e.layer(V + 1)->effectsEnabled);
        e.layer(V + 0)->locked = false;
        // Duplicate a group: its members are copied into the copy
        const int dup = e.duplicateLayer(V + 0);
        CHECK(dup == V + 0 && e.layerCount() == V + 4 && e.layer(V + 0)->isGroup && e.layer(V + 1)->parent == e.layerId(V + 0));
        CHECK(e.layer(V + 2)->isGroup && e.layer(V + 3)->parent == e.layerId(V + 2) && e.layerId(V + 0) != e.layerId(V + 2));
        // Undo / redo of a structure change, and of a removed member
        {
            QUndoStack undo;
            const LayerTree t0 = e.structure();
            undo.push(new cmd::SetStructure(&e, t0, tree::ungrouped(t0, e.layerId(V + 0)), QStringLiteral("Ungroup")));
            CHECK(e.layer(V + 0)->parent == 0 && e.layer(V + 1)->isGroup);
            undo.undo();
            CHECK(e.structure() == t0);
            undo.push(new cmd::RemoveLayer(&e, V + 3));
            CHECK(e.layerCount() == V + 3 && e.groupMembers(V + 2).isEmpty());
            undo.undo();
            CHECK(e.structure() == t0);
            undo.push(new cmd::SetLayerProp(&e, V + 1, cmd::SetLayerProp::Roi, e.layer(V + 1)->roi, QRectF(0, 0, 0.5, 0.5)));
            CHECK(e.layer(V + 1)->roi == QRectF(0, 0, 0.5, 0.5));
            undo.undo();
            undo.push(new cmd::SetLayerProp(&e, V + 1, cmd::SetLayerProp::ColorRemove, QColor(Qt::black), QColor(Qt::red)));
            CHECK(e.layer(V + 1)->color.remove[0] == 1.0f && e.layer(V + 1)->color.remove[1] == 0.0f);
            undo.undo();
            CHECK(e.layer(V + 1)->color.remove[0] == 0.0f);
        }
        // Removing a group keeps its members, at the top level
        e.removeLayer(V + 0);
        CHECK(e.layerCount() == V + 3 && e.layer(V + 0)->parent == 0 && !e.layer(V + 0)->isGroup);

        // Blackout: picture and sound
        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        l = e.addLayer("img");
        e.setLayerImage(l, tmp + "/redblue.png", &err);
        e.setBlackout(true, 0);
        CHECK(e.blackout() && qGray(render().pixel(8, 16)) < 2 && e.audioOutput().fadeLevel() == 0.0f);
        e.fadeCompositionOpacity(1.0, 0); // the master fader does not end the blackout
        CHECK(qGray(render().pixel(8, 16)) < 2);
        e.setBlackout(false, 0);
        CHECK(!e.blackout() && render().pixel(8, 16) != qRgb(0, 0, 0) && e.audioOutput().fadeLevel() == 1.0f);
        // Composition speed: a coefficient on all time, clamped to 0..10; pause holds it at 0 and gives it back
        e.setCompositionSpeed(25);
        CHECK(e.compositionSpeed() == 10.0 && timeScale() == 10.0);
        e.setCompositionSpeed(0.5);
        e.setPaused(true);
        CHECK(timeScale() == 0.0 && e.compositionSpeed() == 0.5);
        e.setPaused(false);
        CHECK(timeScale() == 0.5);
        e.saveProject(tmp + "/cspeed.fulskrin", {}, &err);
        e.newProject();
        CHECK(e.compositionSpeed() == 1.0 && timeScale() == 1.0);
        CHECK(e.loadProject(tmp + "/cspeed.fulskrin", nullptr, &err) && e.compositionSpeed() == 0.5);
        e.newProject();
    }

    // Media Bin: an ISF generator can be imported (kept in the project, not listed as a media)
    {
        e.newProject();
        const QString gen = root + "/../isf/generators/TestPattern.fs";
        e.addBinItems({gen});
        bool inMedia = false;
        for (const auto &r : e.mediaUsage()) inMedia |= r.path.endsWith("TestPattern.fs");
        CHECK(Engine::isIsfFile(gen) && !Engine::isIsfFile("a.mp4") && !inMedia);
        CHECK(e.saveProject(tmp + "/binisf.fulskrin", {}, &err));
        e.newProject();
        CHECK(e.binItems().isEmpty());
        CHECK(e.loadProject(tmp + "/binisf.fulskrin", nullptr, &err) && e.binItems().size() == 1);
        e.newProject();
    }

    // Pivot: the center of the rotation, fixed to the picture
    {
        Mapping m;
        const QSize comp(100, 50);
        m.setCorner(0, QPointF(0.2, 0.2)), m.setCorner(1, QPointF(0.6, 0.2)), m.setCorner(2, QPointF(0.6, 0.6)),
            m.setCorner(3, QPointF(0.2, 0.6));
        CHECK((m.pivotPoint() - QPointF(0.4, 0.4)).manhattanLength() < 1e-9); // the middle by default
        m.setPivotPoint(QPointF(0.2, 0.2)); // the top left corner
        CHECK((m.pivot - QPointF(0, 0)).manhattanLength() < 1e-9);
        m.rotate(90, comp);
        CHECK((m.pivotPoint() - QPointF(0.2, 0.2)).manhattanLength() < 1e-9 && std::abs(m.angle(comp) - 90) < 1e-6);
        CHECK((m.corners[0] - QPointF(0.2, 0.2)).manhattanLength() < 1e-9); // it did not move: it turned about that corner
        Mapping back;
        back.fromJson(m.toJson());
        CHECK((back.pivot - m.pivot).manhattanLength() < 1e-9);
        m.resetCorners();
        CHECK((m.pivot - QPointF(0.5, 0.5)).manhattanLength() < 1e-9);
    }

    // A viewport can be turned: its region is a rectangle of the composition rotated about its center
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 32));
        QImage rb(64, 32, QImage::Format_RGB32); // left half red, right half blue
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) rb.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        rb.save(tmp + "/redblue_vp.png");
        const int v1 = e.viewports().first();
        e.setViewportSize(v1, QSize(32, 32));
        const int li = e.addLayer("img");
        CHECK(e.setLayerImage(li, tmp + "/redblue_vp.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 vid = e.layerId(v1);
        auto shot = [&](double angle) {
            {
                Engine::Lock lk(&e.mutex());
                Mapping &m = e.layer(e.indexOfId(vid))->mapping;
                m.setRect({QPointF(32, 16), 32, 32, angle}, QSize(64, 32));
            }
            for (int k = 0; k < 3; ++k) e.renderFrame();
            return e.grabViewport(vid);
        };
        QImage g = shot(0); // the middle square: red on its left, blue on its right
        CHECK(g.size() == QSize(32, 32) && g.pixelColor(2, 2).red() > 250 && g.pixelColor(29, 2).blue() > 250);
        g = shot(90); // turned a quarter: the halves are now top and bottom, the blue one on top
        CHECK(g.pixelColor(2, 2).blue() > 250 && g.pixelColor(2, 29).red() > 250 && g.pixelColor(29, 2).blue() > 250);
        {
            Engine::Lock lk(&e.mutex());
            const Mapping::Rect r = e.layer(e.indexOfId(vid))->mapping.rect(QSize(64, 32));
            CHECK(std::abs(r.angle - 90) < 1e-6 && std::abs(r.w - 32) < 1e-6 && std::abs(r.center.x() - 32) < 1e-6);
        }
        e.newProject();
    }

    // 4d. OSC and OSCQuery
    {
        osc::Message m{"/a/b", "ifsTFNd", {7, 0.5, QStringLiteral("hey"), true, false, QVariant(), 2.25}};
        std::vector<osc::Message> back;
        CHECK(osc::decode(osc::encode(m), back) && back.size() == 1);
        CHECK(back[0].address == "/a/b" && back[0].types == "ifsTFNd" && back[0].args[0].toInt() == 7 &&
              back[0].args[1].toDouble() == 0.5 && back[0].args[2].toString() == "hey" && back[0].args[3].toBool() &&
              !back[0].args[4].toBool() && back[0].args[6].toDouble() == 2.25);
        QByteArray bundle("#bundle\0\0\0\0\0\0\0\0\1", 16);
        for (const QByteArray &part : {osc::encode({"/x", "i", {1}}), osc::encode({"/y", "s", {QStringLiteral("z")}})}) {
            char n[4];
            qToBigEndian(qint32(part.size()), n);
            bundle += QByteArray(n, 4) + part;
        }
        back.clear();
        CHECK(osc::decode(bundle, back) && back.size() == 2 && back[1].address == "/y");
        CHECK(osc::match("/layer/*/opacity", "/layer/A_b/opacity") && !osc::match("/layer/*/opacity", "/layer/a/b/opacity"));
        CHECK(osc::match("/l/{foo,bar}/[a-c]?", "/l/bar/bx") && !osc::match("/l/{foo,bar}/[!a-c]", "/l/foo/a"));
        CHECK(osc::safeName(" My layer/1 ") == "My_layer_1");

        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        const int li = e.addLayer("My layer");
        e.setLayerImage(li, tmp + "/redblue.png", &err);
        e.addEffect(li, isf + "/effects/FlipCrop.fs", &err);
        const int gi = e.addGroup("G", V + 0);
        e.setStructure(tree::intoGroup(e.structure(), {e.layerId(V + 1)}, e.layerId(gi)));
        OscServer server(&e);
        CHECK(server.start(0, 0, "test", false));
        CHECK(server.oscPort() > 0 && server.queryPort() > 0);
        const QString L = "/layer/G/layer/My_layer";
        CHECK(server.handleMessage({L + "/opacity", "f", {0.25}}) && std::abs(e.layer(V + 1)->opacity - 0.25f) < 1e-6);
        CHECK(server.handleMessage({"/layer/G/layer/*/opacity", "f", {0.5}}) && std::abs(e.layer(V + 1)->opacity - 0.5f) < 1e-6);
        CHECK(server.handleMessage({L + "/color/add", "fff", {0.1, 0.2, 0.3}}) && std::abs(e.layer(V + 1)->color.add[2] - 0.3f) < 1e-6);
        CHECK(server.handleMessage({L + "/color/temp", "f", {-2000.0}}) && e.layer(V + 1)->color.temp == -2000.0f);
        CHECK(server.handleMessage({L + "/color/tint", "f", {500.0}}) && e.layer(V + 1)->color.tint == 100.0f); // clipped
        CHECK(server.handleMessage({L + "/roi/left", "f", {0.25}}) && std::abs(e.layer(V + 1)->roi.left() - 0.25) < 1e-9);
        CHECK(server.handleMessage({L + "/fx/FlipCrop/param/flipH", "T", {true}}));
        CHECK(e.layer(V + 1)->effects[0]->inputs()[1].bValue || e.layer(V + 1)->effects[0]->inputs()[2].bValue);
        CHECK(server.handleMessage({L + "/fx/enable", "F", {false}}) && !e.layer(V + 1)->effectsEnabled);
        CHECK(server.handleMessage({"/layer/G/opacity", "i", {0}}) && e.layer(V + 0)->opacity == 0.0f);
        CHECK(server.handleMessage({"/composition/blackout", "T", {true}}) && e.blackout());
        server.handleMessage({"/composition/blackout", "i", {0}});
        CHECK(!e.blackout());
        // Locked group: its member refuses edits, visibility still works
        CHECK(server.handleMessage({"/layer/G/locked", "T", {true}}) && e.isLocked(V + 1));
        CHECK(!server.handleMessage({L + "/opacity", "f", {0.9}}) && std::abs(e.layer(V + 1)->opacity - 0.5f) < 1e-6);
        CHECK(server.handleMessage({L + "/enable", "F", {false}}) && !e.layer(V + 1)->enabled);
        server.handleMessage({"/layer/G/locked", "F", {false}});
        // A viewport: position and size in composition pixels (no scale)
        {
            const int vi = e.indexOfId(e.mainViewportId());
            const QString VP = "/viewport/" + osc::safeName(e.layer(vi)->name);
            CHECK(server.handleMessage({VP + "/spatial/width", "f", {32}}) && server.handleMessage({VP + "/spatial/height", "f", {16}}));
            CHECK(server.handleMessage({VP + "/spatial/position/x", "f", {10}}) && server.handleMessage({VP + "/spatial/position/y", "f", {8}}));
            const QRectF bb = e.layer(vi)->mapping.bounds();
            CHECK(std::abs(bb.width() * 64 - 32) < 1e-6 && std::abs(bb.height() * 32 - 16) < 1e-6);
            CHECK(std::abs(bb.center().x() * 64 - 10) < 1e-6 && std::abs(bb.center().y() * 32 - 8) < 1e-6);
            CHECK(!server.handleMessage({VP + "/spatial/scale", "ff", {50, 50}}));
            CHECK(server.handleMessage({VP + "/spatial/pivot/x", "f", {0}}) && server.handleMessage({VP + "/spatial/pivot/y", "f", {0}}));
            CHECK(server.handleMessage({VP + "/spatial/rotation", "f", {90}}));
            {
                const Mapping &pm = e.layer(vi)->mapping;
                CHECK((pm.pivotPoint() - QPointF(0, 0)).manhattanLength() < 1e-6 && std::abs(pm.angle(e.compositionSize()) - 90) < 1e-6);
            }
            QStringList paths;
            for (const auto &p : e.animatableParams(e.layerId(vi))) paths << p.path;
            CHECK(paths.contains("spatial/width") && paths.contains("spatial/height") && paths.contains("spatial/position/x") &&
                  !paths.contains("spatial/scale/x") && paths.contains("spatial/rotation") &&
                  paths.contains("spatial/pivot/x") && paths.contains("spatial/pivot/y"));
            e.layer(vi)->mapping.resetCorners();
        }
        // Renaming changes the address
        CHECK(server.handleMessage({L + "/name", "s", {QStringLiteral("Front wall")}}) && e.layer(V + 1)->name == "Front wall");
        int status = 0;
        QJsonObject v = QJsonDocument::fromJson(server.httpGet("/layer/G/layer/Front_wall/opacity?VALUE", &status)).object();
        CHECK(status == 200 && v.value("VALUE").toArray().at(0).toDouble() == 0.5);
        server.httpGet(L + "/opacity", &status);
        CHECK(status == 404);
        QJsonObject root = QJsonDocument::fromJson(server.httpGet("/", &status)).object();
        const QJsonObject op = root["CONTENTS"].toObject()["layer"].toObject()["CONTENTS"].toObject()["G"].toObject()["CONTENTS"]
                                   .toObject()["layer"].toObject()["CONTENTS"].toObject()["Front_wall"].toObject()["CONTENTS"]
                                   .toObject()["opacity"].toObject();
        CHECK(op.value("TYPE").toString() == "f" && op.value("ACCESS").toInt() == 3 &&
              op.value("RANGE").toArray().at(0).toObject().value("MAX").toDouble() == 1.0);
        const QJsonObject compo = root["CONTENTS"].toObject()["composition"].toObject()["CONTENTS"].toObject();
        CHECK(!root["CONTENTS"].toObject().contains("master"));
        for (const char *k : {"opacity", "volume", "blackout", "width", "height", "fps"}) CHECK(compo.contains(k));
        CHECK(!compo.contains("mute") && compo["blackout"].toObject()["CONTENTS"].toObject().contains("fade"));
        CHECK(server.handleMessage({"/composition/opacity", "f", {0.4}}) && std::abs(e.compositionOpacityTarget() - 0.4) < 1e-3);
        e.fadeCompositionOpacity(1.0, 0);
        CHECK(server.handleMessage({"/composition/fps", "f", {30.0}}) && e.effectiveRender().frameRate == 30.0);
        e.setRenderSettings(Engine::RenderSettings());
        // Hierarchy, lowercase, and an "enable" child for every switch, over the whole tree
        {
            int bad = 0, nodes = 0;
            std::function<void(const QJsonObject &, const QString &)> walk = [&](const QJsonObject &n, const QString &parent) {
                const QJsonObject c = n.value("CONTENTS").toObject();
                for (auto it = c.begin(); it != c.end(); ++it) {
                    ++nodes;
                    const QString seg = it.key();
                    const bool userName = parent == "layer" || parent == "viewport" || parent == "fx" || parent == "param"; // names: layers, viewports, effects, shader parameters
                    if (!userName && seg != seg.toLower()) { ++bad; qWarning("not lowercase: %s", qPrintable(it.value().toObject().value("FULL_PATH").toString())); }
                    if (!userName && seg.endsWith("s") && seg != "fps") { ++bad; qWarning("plural: %s", qPrintable(it.value().toObject().value("FULL_PATH").toString())); }
                    if (!userName && (seg.endsWith("enabled") || seg.endsWith("Enabled") || seg.endsWith("On") || seg == "master")) {
                        ++bad;
                        qWarning("switch not named enable: %s", qPrintable(it.value().toObject().value("FULL_PATH").toString()));
                    }
                    walk(it.value().toObject(), userName ? QStringLiteral("*") : seg);
                }
            };
            walk(root, QString());
            CHECK(nodes > 50 && bad == 0);
            const QString LF = "/layer/G/layer/Front_wall";
            CHECK(server.handleMessage({LF + "/color/tint/enable", "T", {true}}) && e.layer(V + 1)->color.tintOn);
            CHECK(server.handleMessage({LF + "/color/mask/invert", "T", {true}}) && e.layer(V + 1)->color.maskInvert);
            CHECK(server.handleMessage({LF + "/blend_mode", "s", {QStringLiteral("add")}}) && e.layer(V + 1)->blend == BlendMode::Add);
            CHECK(server.handleMessage({LF + "/spatial/rotation", "f", {30.0}}) &&
                  std::abs(e.layer(V + 1)->mapping.angle(e.compositionSize()) - 30.0) < 0.01);
            e.layer(V + 1)->blend = BlendMode::Normal;
        }
        const QJsonObject host = QJsonDocument::fromJson(server.httpGet("/?HOST_INFO", &status)).object();
        CHECK(host.value("OSC_PORT").toInt() == server.oscPort() && host.value("EXTENSIONS").toObject().value("LISTEN").toBool());

        auto pump = [&](const std::function<bool()> &done) {
            QElapsedTimer t;
            t.start();
            while (!done() && t.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(2);
            }
            return done();
        };
        // Real UDP message
        const QString F = "/layer/G/layer/Front_wall";
        QUdpSocket udp;
        udp.writeDatagram(osc::encode({F + "/opacity", "f", {0.75}}), QHostAddress::LocalHost, server.oscPort());
        CHECK(pump([&] { return std::abs(e.layer(V + 1)->opacity - 0.75f) < 1e-6; }));
        // Real HTTP request
        QTcpSocket http;
        http.connectToHost(QHostAddress::LocalHost, server.queryPort());
        CHECK(pump([&] { return http.state() == QAbstractSocket::ConnectedState; }));
        http.write(QByteArray("GET ") + (F + "/opacity?VALUE").toUtf8() + " HTTP/1.1\r\nHost: x\r\n\r\n");
        QByteArray reply;
        pump([&] {
            reply += http.readAll();
            return reply.contains("VALUE");
        });
        CHECK(reply.startsWith("HTTP/1.1 200") && reply.contains("[0.75]"));
        // WebSocket: LISTEN, then a change is pushed as a binary OSC message
        QTcpSocket ws;
        ws.connectToHost(QHostAddress::LocalHost, server.queryPort());
        CHECK(pump([&] { return ws.state() == QAbstractSocket::ConnectedState; }));
        ws.write("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        QByteArray wsIn;
        CHECK(pump([&] {
            wsIn += ws.readAll();
            return wsIn.contains("\r\n\r\n");
        }));
        CHECK(wsIn.contains("101") && wsIn.contains("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
        wsIn.clear();
        auto wsSend = [&](int opcode, const QByteArray &payload) { // masked client frame
            QByteArray f;
            f.append(char(0x80 | opcode));
            f.append(char(0x80 | payload.size()));
            const char mask[4] = {1, 2, 3, 4};
            f.append(mask, 4);
            for (int k = 0; k < payload.size(); ++k) f.append(char(payload[k] ^ mask[k & 3]));
            ws.write(f);
        };
        wsSend(1, QByteArray("{\"COMMAND\":\"LISTEN\",\"DATA\":\"") + (F + "/opacity").toUtf8() + "\"}");
        QCoreApplication::processEvents();
        wsSend(2, osc::encode({F + "/opacity", "f", {0.125}})); // an OSC message through the WebSocket
        osc::Message got;
        CHECK(pump([&] {
            wsIn += ws.readAll();
            while (wsIn.size() >= 2) {
                const int len = wsIn[1] & 0x7F;
                if (wsIn.size() < 2 + len) break;
                std::vector<osc::Message> ms;
                if (osc::decode(wsIn.mid(2, len), ms) && !ms.empty()) got = ms.back();
                wsIn.remove(0, 2 + len);
            }
            return got.args.value(0).toDouble() == 0.125;
        }));
        CHECK(got.address == F + "/opacity" && got.types == "f");
        // A new layer: the clients are told to fetch the tree again
        e.addLayer("Late");
        bool changed = false;
        CHECK(pump([&] {
            wsIn += ws.readAll();
            changed |= wsIn.contains("PATH_CHANGED");
            return changed;
        }));
        {
            int st = 0;
            const QJsonObject layers = QJsonDocument::fromJson(server.httpGet("/layer", &st)).object();
            CHECK(layers["CONTENTS"].toObject().contains("Late") && layers["CONTENTS"].toObject()["Late"].toObject()["DESCRIPTION"].toString() == "Late");
        }
        ws.close();
        http.close();
        server.stop();

        // Zeroconf responder: answers to a PTR question for _oscjson._tcp.local (without the network)
        {
            Zeroconf z;
            z.start("Fulskrin (test)", {{"_oscjson._tcp", 5678, {{"txtvers", "1"}}}, {"_osc._udp", 1234, {}}});
            QByteArray q;
            const char head[12] = {0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            q.append(head, 12);
            for (const char *label : {"_oscjson", "_tcp", "local"}) {
                q.append(char(std::strlen(label)));
                q.append(label);
            }
            q.append('\0');
            q.append("\x00\x0c\x00\x01", 4); // PTR, IN
            bool uni = false;
            const QByteArray a = z.answerQuery(q, &uni);
            CHECK(a.size() > 12 && quint8(a[0]) == 0x12 && quint8(a[2]) == 0x84 && a.contains("Fulskrin (test)"));
            CHECK(a.contains(QByteArray("\x16\x2e", 2))); // SRV port 5678 in the additional records
            CHECK(a.contains(z.hostName().toUtf8()) && a.contains("txtvers=1"));
            q[q.size() - 3] = 0x10; // TXT of another name: no answer
            CHECK(z.answerQuery(QByteArray("\x00\x00\x84\x00", 4) + q.mid(4)).isEmpty()); // a response is ignored
            CHECK(z.announcement(true).contains("_osc"));
            z.stop();
        }
        e.newProject();
    }

    // 4e. Snapshots: capture, recall (instant and with a fade), recreation, lock, save / load
    {
        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        const int a = e.addLayer("A");
        e.setLayerImage(a, tmp + "/redblue.png", &err);
        const int b = e.addLayer("B", V + 1);
        e.setLayerIsf(b, isf + "/generators/SolidColor.fs", &err);
        e.layer(V + 0)->opacity = 0.8f;
        e.layer(V + 1)->enabled = false;
        Engine::Snapshot m;
        m.name = "Look 1";
        m.fade = 0;
        m.layers = e.captureLayers();
        m.thumbnail = QImage(16, 9, QImage::Format_RGB32);
        m.thumbnail.fill(Qt::red);
        CHECK(e.addSnapshot(m) == 0 && e.snapshotCount() == 1);
        // Change things, then recall
        e.layer(V + 0)->opacity = 0.2f;
        e.layer(V + 0)->color.temp = 1000;
        e.layer(V + 0)->mapping.setCorner(0, QPointF(0.3, 0.3));
        e.layer(V + 1)->enabled = true;
        e.addEffect(V + 0, isf + "/effects/FlipCrop.fs", &err);
        e.recallSnapshot(0);
        CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6 && e.layer(V + 0)->color.temp == 0.0f);
        CHECK(e.layer(V + 0)->mapping.corners[0] == QPointF(0, 0) || e.layer(V + 0)->mapping.corners[0].y() > 0.0);
        CHECK(e.layer(V + 0)->effects.empty() && !e.layer(V + 1)->enabled);
        // With a fade: halfway between, then the target; a layer becoming visible fades in from 0
        Engine::Snapshot m2 = m;
        m2.name = "Look 2";
        m2.fade = 0.4;
        e.layer(V + 0)->opacity = 0.0f;
        e.layer(V + 1)->enabled = true;
        m2.layers = e.captureLayers();
        e.addSnapshot(m2);
        e.recallSnapshot(0); // back to look 1, at once
        e.recallSnapshot(1);
        CHECK(e.isFading() && e.layer(V + 1)->enabled && e.layer(V + 1)->opacity < 0.05f);
        QElapsedTimer ft;
        ft.start();
        bool sawMiddle = false;
        while (e.isFading() && ft.elapsed() < 3000) {
            e.renderFrame();
            sawMiddle |= e.layer(V + 0)->opacity > 0.1f && e.layer(V + 0)->opacity < 0.7f;
            QThread::msleep(10);
        }
        CHECK(!e.isFading() && sawMiddle && e.layer(V + 0)->opacity == 0.0f && std::abs(e.layer(V + 1)->opacity - 1.0f) < 1e-6);
        // A layer removed since is recreated; a locked layer is left alone
        e.removeLayer(V + 1);
        e.layer(V + 0)->locked = true;
        e.layer(V + 0)->opacity = 0.5f;
        e.recallSnapshot(0);
        CHECK(e.layerCount() == V + 2 && e.layer(V + 1)->name == "B" && std::abs(e.layer(V + 0)->opacity - 0.5f) < 1e-6);
        e.layer(V + 0)->locked = false;
        // Undo of a recall
        {
            QUndoStack undo;
            e.layer(V + 0)->opacity = 0.33f;
            undo.push(new cmd::RecallSnapshot(&e, 0));
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6);
            undo.undo();
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.33f) < 1e-6);
        }
        // Each value on its own time: opacity cut, temperature 2 s, the others on the snapshot's fade (1 s)
        {
            const int ti = e.addLayer("Timed", e.layerCount());
            const quint64 tid = e.layerId(ti);
            auto L = [&] { return e.layer(e.indexOfId(tid)); };
            L()->opacity = 1.0f;
            L()->color.temp = 1000;
            L()->roi = QRectF(0, 0, 0.5, 1);
            QJsonObject state = e.layerJson(e.indexOfId(tid));
            state["timing"] = QJsonObject{{"opacity", 0}, {"color/temp", 2.0}};
            L()->opacity = 0.0f;
            L()->color.temp = 0;
            L()->roi = QRectF(0, 0, 1, 1);
            e.applyLayers(QJsonArray{state}, 1.0);
            CHECK(L()->opacity == 1.0f && L()->color.temp < 1 && std::abs(L()->roi.width() - 1.0) < 1e-9); // the cut, at once
            e.advanceFades(0.5);
            CHECK(L()->roi.width() < 0.99 && L()->roi.width() > 0.51 && L()->color.temp > 1 && L()->color.temp < 400);
            e.advanceFades(0.6); // 1.1 s: the fade is over, the temperature is not
            CHECK(std::abs(L()->roi.width() - 0.5) < 1e-9 && L()->color.temp > 400 && L()->color.temp < 999 && e.isFading());
            e.advanceFades(1.0);
            CHECK(std::abs(L()->color.temp - 1000) < 1e-3 && !e.isFading());
            // Hidden by a cut while the rest fades: at once
            state["enable"] = false;
            e.applyLayers(QJsonArray{state}, 1.0);
            CHECK(!L()->enabled && L()->opacity == 1.0f);
            // A time of its own with no snapshot fade at all
            L()->enabled = true;
            L()->color.temp = 0;
            e.applyLayers(QJsonArray{state.value("enable").toBool() ? state : [&] { QJsonObject x = state; x["enable"] = true; return x; }()}, 0.0);
            CHECK(L()->color.temp < 1 && e.isFading());
            e.advanceFades(2.0);
            CHECK(std::abs(L()->color.temp - 1000) < 1e-3 && !e.isFading());
            CHECK(Engine::timingKey({"roi", "2"}, {}) == "roi" && Engine::timingKey({"color", "add", "1"}, {}) == "color/add" &&
                  Engine::timingKey({"fx", "0", "params", "radius"}, QJsonObject{{"fx", QJsonArray{QJsonObject{{"path", "/x/Blur.fs"}}}}}) == "fx/Blur/param/radius" &&
                  Engine::timingKey({"source", "speed"}, {}) == "source/speed" && Engine::timingKey({"source", "file"}, {}).isEmpty());
            // The pivot: a cut by default, with a time and a curve of its own when the snapshot gives them
            {
                CHECK(Engine::timingKey({"spatial", "pivot", "0"}, {}) == "spatial/pivot" &&
                      Engine::timingKey({"spatial", "corners", "0", "0"}, {}) == "spatial");
                QJsonObject ps = e.layerJson(e.indexOfId(tid));
                QJsonObject sp = ps.value("spatial").toObject();
                sp["pivot"] = QJsonArray{0.0, 0.0};
                ps["spatial"] = sp;
                ps.remove("timing");
                L()->mapping.pivot = QPointF(0.5, 0.5);
                e.applyLayers(QJsonArray{ps}, 1.0); // the fade is 1 s, the pivot ignores it
                CHECK((L()->mapping.pivot - QPointF(0, 0)).manhattanLength() < 1e-9);
                e.advanceFades(2.0);
                ps["timing"] = QJsonObject{{"spatial/pivot", 2.0}};
                sp["pivot"] = QJsonArray{1.0, 1.0};
                ps["spatial"] = sp;
                e.applyLayers(QJsonArray{ps}, 0.0);
                CHECK((L()->mapping.pivot - QPointF(0, 0)).manhattanLength() < 1e-9 && e.isFading());
                e.advanceFades(1.0);
                CHECK(L()->mapping.pivot.x() > 0.05 && L()->mapping.pivot.x() < 0.95);
                e.advanceFades(1.5);
                CHECK((L()->mapping.pivot - QPointF(1, 1)).manhattanLength() < 1e-9 && !e.isFading());
            }
            // Text generator: the snapshot's text is typed over its time (erased back to what both share, then
            // typed); its style moves on its own times
            {
                const int xi = e.addLayer("Words", e.layerCount());
                const quint64 xid = e.layerId(xi);
                auto X = [&] { return e.layer(e.indexOfId(xid)); };
                CHECK(e.setLayerText(xi) && X()->type == SourceType::Text);
                e.setLayerTextContent(xi, "Hello");
                e.editLayerText(xi, [](Layer &l) {
                    l.text.size = 40;
                    l.text.color = Qt::white;
                });
                QJsonObject st = e.layerJson(e.indexOfId(xid));
                QJsonObject src = st.value("source").toObject();
                CHECK(src.value("type") == "text" && src.value("color").isArray() && src.value("h_align") == "center");
                src["content"] = "Help me";
                src["size"] = 80;
                src["bold"] = true;
                src["color"] = QJsonArray{1, 0, 0, 1};
                st["source"] = src;
                st["timing"] = QJsonObject{{"source/text/size", 2.0}, {"source/text/color", 0}};
                e.applyLayers(QJsonArray{st}, 1.0);
                CHECK(X()->text.content == "Help me" && X()->text.shown() == "Hello" && X()->text.bold);
                CHECK(X()->text.color == QColor(255, 0, 0)); // a cut
                e.advanceFades(0.5); // 6 steps (erase "lo", type "p me") at an even pace: 3 done
                CHECK(X()->text.shown() == "Help" && X()->text.size > 40 && X()->text.size < 80);
                e.advanceFades(0.6);
                CHECK(X()->text.shown() == "Help me" && X()->text.size < 80 && e.isFading());
                e.advanceFades(1.0);
                CHECK(X()->text.size == 80 && !e.isFading());
                // Typed by hand: no typewriter
                e.setLayerTextContent(xi, "Hi");
                CHECK(X()->text.shown() == "Hi");
                // A cut types nothing
                src["content"] = "Cut";
                st["source"] = src;
                st["timing"] = QJsonObject{{"source/text/content", 0}};
                e.applyLayers(QJsonArray{st}, 1.0);
                CHECK(X()->text.shown() == "Cut");
                e.advanceFades(3.0);
                CHECK(Engine::timingKey({"source", "color", "2"}, {}) == "source/text/color" &&
                      Engine::timingKey({"source", "content"}, {}) == "source/text/content" &&
                      Engine::timingKey({"source", "params", "size"}, {}) == "source/param/size");
                // Kept in range when read from a file
                src["size"] = 100000;
                src["line_height"] = -3;
                st["source"] = src;
                st.remove("timing");
                e.applyLayers(QJsonArray{st}, 0.0);
                CHECK(X()->text.size == 1000 && X()->text.lineHeight > 0);
                e.removeLayer(e.indexOfId(xid));
            }
            // The composition in a snapshot: its opacity and the sound's volume fade on their times
            {
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
                QJsonObject c = e.captureComposition();
                CHECK(c.value("included").toBool() && c.value("opacity").toDouble() == 1.0 && c.contains("volume") && !c.contains("muted"));
                c["opacity"] = 0.0;
                c["volume"] = 0.5;
                c["timing"] = QJsonObject{{"volume", 0}};
                e.applyComposition(c, 1.0);
                CHECK(std::abs(e.audioVolume() - 0.5f) < 1e-6 && e.compositionOpacityTarget() > 0.99);
                e.advanceFades(0.5);
                CHECK(e.compositionOpacityTarget() > 0.05 && e.compositionOpacityTarget() < 0.95);
                e.advanceFades(0.6);
                CHECK(e.compositionOpacityTarget() == 0.0);
                // Left out: nothing moves
                c["included"] = false;
                c["opacity"] = 1.0;
                e.applyComposition(c, 0.0);
                CHECK(e.compositionOpacityTarget() == 0.0);
                // Saved with the project
                Engine::Snapshot mc;
                mc.name = "Comp";
                mc.composition = c;
                const int mi = e.addSnapshot(mc);
                CHECK(e.saveProject(tmp + "/comp.fulskrin", {}, &err));
                CHECK(readJson(tmp + "/comp.fulskrin").value("snapshots").toArray().at(mi).toObject().value("composition") == c);
                // Every key of a project is lowercase snake_case (parameter names of shaders and the times of a snapshot aside)
                {
                    int bad = 0;
                    std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &v) {
                        if (v.isArray()) {
                            for (const QJsonValue &x : v.toArray()) walk(x);
                        } else if (v.isObject()) {
                            const QJsonObject o = v.toObject();
                            for (auto it = o.begin(); it != o.end(); ++it) {
                                static const QRegularExpression ok("^[a-z0-9_]+$");
                                if (!ok.match(it.key()).hasMatch()) { ++bad; qWarning("project key: %s", qPrintable(it.key())); }
                                if (it.key() != "params" && it.key() != "timing") walk(it.value());
                            }
                        }
                    };
                    walk(readJson(tmp + "/comp.fulskrin"));
                    CHECK(bad == 0);
                }
                e.removeSnapshot(mi);
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
            }
            // Soft edge (crop) and per-viewport opacity: stored, recalled, and faded
            {
                const quint64 vp = 4242;
                L()->enabled = true;
                L()->mapping.soft.enabled = true;
                L()->mapping.soft.width[0] = 0.4;
                L()->viewportOpacity[vp] = 0.2f;
                QJsonObject s2 = e.layerJson(e.indexOfId(tid));
                s2.remove("timing");
                L()->mapping.soft.enabled = false;
                L()->mapping.soft.width[0] = 0.0;
                L()->viewportOpacity.clear();
                e.applyLayers(QJsonArray{s2}, 0.0); // a cut
                CHECK(L()->mapping.soft.enabled && std::abs(L()->mapping.soft.width[0] - 0.4) < 1e-6);
                CHECK(L()->viewportOpacity.count(vp) && std::abs(L()->viewportOpacity.at(vp) - 0.2f) < 1e-6);
                L()->mapping.soft.width[0] = 0.0;
                L()->viewportOpacity.clear();
                e.applyLayers(QJsonArray{s2}, 1.0); // a fade
                e.advanceFades(0.5);
                CHECK(L()->mapping.soft.width[0] > 0.01 && L()->mapping.soft.width[0] < 0.39);
                CHECK(L()->viewportOpacity.count(vp) && L()->viewportOpacity.at(vp) > 0.21f && L()->viewportOpacity.at(vp) < 0.99f);
                e.advanceFades(0.6);
                CHECK(std::abs(L()->mapping.soft.width[0] - 0.4) < 1e-6 && std::abs(L()->viewportOpacity.at(vp) - 0.2f) < 1e-6);
            }
            e.removeLayer(e.indexOfId(tid));
        }
        // ISF speed (generator and effect) fades between snapshots instead of jumping
        {
            const int gi = e.addLayer("Pace", e.layerCount());
            const quint64 gid = e.layerId(gi);
            auto G = [&] { return e.layer(e.indexOfId(gid)); };
            CHECK(e.setLayerIsf(gi, QStringLiteral(TEST_DIR) + "/../isf/generators/TestPattern.fs", &err));
            G()->enabled = true;
            G()->generator->speed = 3.0;
            QJsonObject st = e.layerJson(e.indexOfId(gid));
            st.remove("timing");
            G()->generator->speed = 1.0;
            e.applyLayers(QJsonArray{st}, 1.0);
            CHECK(std::abs(G()->generator->speed - 1.0) < 1e-9); // starts from where it is
            e.advanceFades(0.5);
            CHECK(G()->generator->speed > 1.05 && G()->generator->speed < 2.95);
            e.advanceFades(0.6);
            CHECK(std::abs(G()->generator->speed - 3.0) < 1e-9);
            e.removeLayer(e.indexOfId(gid));
        }
        // Another source by a snapshot: the outgoing one stays, invisible, and a transition mixes the two
        {
            for (const auto &[name, rgb] : {std::pair<QString, QRgb>{"red", qRgb(255, 0, 0)}, {"blue", qRgb(0, 0, 255)}}) {
                QImage im(32, 16, QImage::Format_RGB32);
                im.fill(rgb);
                im.save(tmp + "/" + name + ".png");
            }
            const int ti = e.addLayer("Trans", V);
            const quint64 tid = e.layerId(ti);
            CHECK(e.setLayerImage(ti, tmp + "/red.png", &err));
            e.layer(ti)->mapping.resetCorners();
            e.layer(ti)->roi = QRectF(0, 0, 0.5, 1); // the snapshot's ROI: the incoming source gets there over the time
            QJsonObject red = e.layerJson(ti);
            e.layer(ti)->roi = QRectF(0, 0, 1, 1);
            red["included"] = true;
            CHECK(e.setLayerImage(ti, tmp + "/blue.png", &err));
            const quint64 vp = e.mainViewportId();
            auto center = [&] {
                e.renderFrame();
                const QImage g = e.grabViewport(vp);
                return g.pixelColor(g.width() / 2, g.height() / 2);
            };
            CHECK(center().blue() > 250);
            e.setFadesManual(true); // the frames below do not move the transition: only advanceFades
            e.applyLayers(QJsonArray{red}, 1.0);
            CHECK(e.isTransitioning(tid) && e.layerJson(e.indexOfId(tid)).value("source").toObject().value("path").toString().endsWith("red.png"));
            e.advanceFades(0.5);
            CHECK(std::abs(e.layer(e.indexOfId(tid))->transitionGain - 0.5f) < 0.01f); // the sound crosses too
            CHECK(e.layer(e.indexOfId(tid))->roi.width() > 0.6 && e.layer(e.indexOfId(tid))->roi.width() < 0.9); // no cut
            QColor c = center();
            CHECK(c.red() > 90 && c.red() < 170 && c.blue() > 90 && c.blue() < 170); // halfway: both
            e.advanceFades(0.6);
            c = center();
            CHECK(!e.isTransitioning(tid) && c.red() > 250 && c.blue() < 5 && e.layer(e.indexOfId(tid))->transitionGain == 1.0f);
            CHECK(std::abs(e.layer(e.indexOfId(tid))->roi.width() - 0.5) < 1e-6);
            // An ISF transition chosen for the layer: fade out, fade in — halfway, only what is beneath shows
            e.layer(e.indexOfId(tid))->enabled = false;
            const QColor under = center();
            e.layer(e.indexOfId(tid))->enabled = true;
            QJsonObject blue = e.layerJson(e.indexOfId(tid));
            blue["source"] = [&] {
                QJsonObject src = blue.value("source").toObject();
                src["path"] = tmp + "/blue.png";
                src["transition"] = root + "/../isf/transitions/FadeOutIn.fs";
                return src;
            }();
            e.applyLayers(QJsonArray{blue}, 1.0);
            e.advanceFades(0.5);
            c = center();
            CHECK(e.isTransitioning(tid) && std::abs(c.red() - under.red()) < 30 && std::abs(c.blue() - under.blue()) < 30);
            e.advanceFades(0.6);
            CHECK(center().blue() > 250);
            // The source's own time: a cut, no transition
            red["timing"] = QJsonObject{{"source/file", 0}};
            e.applyLayers(QJsonArray{red}, 1.0);
            CHECK(!e.isTransitioning(tid) && center().red() > 250);
            // A transition running when the layer goes: it goes with it
            blue["timing"] = QJsonObject{{"source/file", 2.0}};
            e.applyLayers(QJsonArray{blue}, 0.0); // its own time even with no snapshot fade
            CHECK(e.isTransitioning(tid));
            e.removeLayer(e.indexOfId(tid));
            CHECK(!e.isTransitioning(tid));
            e.setFadesManual(false);
        }
        // Saved with the project
        CHECK(e.saveProject(tmp + "/snapshots.fulskrin", {}, &err));
        CHECK(e.loadProject(tmp + "/snapshots.fulskrin", nullptr, &err));
        CHECK(e.snapshotCount() == 2 && e.snapshot(1).name == "Look 2" && std::abs(e.snapshot(1).fade - 0.4) < 1e-9);
        CHECK(e.snapshot(0).thumbnail.width() == 16 && e.snapshot(0).layers.size() == 2);
        e.recallSnapshot(0);
        CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6);
        // Snapshots have ids of their own, kept by edits and by the project
        CHECK(e.snapshot(0).id && e.snapshot(1).id && e.snapshot(0).id != e.snapshot(1).id);
        {
            const quint64 id0 = e.snapshot(0).id;
            Engine::Snapshot m = e.snapshot(0);
            m.id = 999;
            e.setSnapshot(0, m);
            CHECK(e.snapshot(0).id == id0 && e.indexOfSnapshot(id0) == 0 && e.indexOfSnapshot(12345) == -1);
            e.addSnapshot(e.snapshot(1)); // a copy: another id
            CHECK(e.snapshot(2).id != e.snapshot(1).id);
            e.removeSnapshot(2);
        }
        // Sequences: steps recalling snapshots, GO / GO BACK, loop, saved with the project
        {
            Engine::Sequence seq;
            seq.name = "Show";
            seq.steps = {{e.snapshot(1).id, "Act 1"}, {e.snapshot(0).id, "Act 2"}, {987654, "gone"}};
            CHECK(e.addSequence(seq) == 0 && e.currentSequence() == 0 && e.sequencePosition() == -1);
            CHECK(e.sequenceNext() == 0 && e.sequencePrevious() == -1);
            e.layer(V + 0)->opacity = 0.1f;
            CHECK(e.sequenceGo() && e.sequencePosition() == 0); // snapshot 1 ("Look 2"): layer 0 at 0, with its fade
            CHECK(e.sequenceGo() && e.sequencePosition() == 1 && std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6); // snapshot 0, at once
            CHECK(e.sequenceGo() && e.sequencePosition() == 2); // its snapshot is gone: the position moves, nothing else
            CHECK(e.sequenceNext() == -1 && !e.sequenceGo()); // the end, no loop
            seq.loop = true;
            e.setSequence(0, seq);
            CHECK(e.sequenceNext() == 0);
            CHECK(e.sequenceBack() && e.sequencePosition() == 1);
            e.setSequencePosition(0);
            CHECK(e.sequencePrevious() == 2); // loop: before the first, the last
            Engine::Sequence copy = e.sequence(0);
            copy.name = "Copy";
            CHECK(e.addSequence(copy) == 1 && e.currentSequence() == 0);
            e.setCurrentSequence(1);
            CHECK(e.sequencePosition() == -1);
            CHECK(e.saveProject(tmp + "/sequences.fulskrin", {}, &err));
            CHECK(e.loadProject(tmp + "/sequences.fulskrin", nullptr, &err));
            CHECK(e.sequenceCount() == 2 && e.currentSequence() == 1 && e.sequence(0).loop &&
                  e.sequence(0).steps.size() == 3 && e.sequence(0).steps[0].text == "Act 1" &&
                  e.sequence(0).steps[0].snapshot == e.snapshot(1).id);
            e.removeSequence(1);
            e.removeSequence(0);
            CHECK(e.sequenceCount() == 0 && e.currentSequence() == -1 && !e.sequenceGo());
        }
        // Snapshots run side by side: a recall takes over only the values it holds, from where they are
        {
            e.setFadesManual(true);
            const int ai = e.addLayer("Side A", V);
            const quint64 ia = e.layerId(ai);
            const int bi = e.addLayer("Side B", V);
            const quint64 ib = e.layerId(bi);
            auto op = [&](quint64 id) { return e.layer(e.indexOfId(id))->opacity; };
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            {
                Engine::Lock lk(&e.mutex());
                for (quint64 id : {ia, ib}) {
                    e.layer(e.indexOfId(id))->opacity = 0;
                    e.layer(e.indexOfId(id))->enabled = true;
                }
            }
            // A snapshot holding one layer (the others are known, left alone)
            auto memWith = [&](quint64 id, double opacity, double fade) {
                Engine::Snapshot m;
                m.name = "Side";
                m.fade = fade;
                for (const QJsonValue &v : e.captureLayers()) {
                    QJsonObject o = v.toObject();
                    const bool me = o.value("id").toString().toULongLong() == id;
                    o["included"] = me;
                    if (me) o["opacity"] = opacity;
                    m.layers.append(o);
                }
                return e.addSnapshot(m);
            };
            const int mA1 = memWith(ia, 1.0, 2.0), mB1 = memWith(ib, 1.0, 2.0), mA0 = memWith(ia, 0.0, 2.0);
            e.recallSnapshot(mA1);
            e.advanceFades(1.0);
            CHECK(near(op(ia), 0.5)); // eased: half way at half the time
            e.recallSnapshot(mB1);       // another layer: A goes on
            e.advanceFades(1.0);
            CHECK(near(op(ia), 1.0) && near(op(ib), 0.5));
            e.recallSnapshot(mA0); // A again: this one takes it over
            e.advanceFades(1.0);
            CHECK(near(op(ia), 0.5) && near(op(ib), 1.0));
            e.recallSnapshot(mA1); // in the middle of a fade: from where it is
            e.advanceFades(1.0);
            CHECK(near(op(ia), 0.75));
            e.advanceFades(5.0);
            CHECK(near(op(ia), 1.0) && !e.isFading());

            // The composition: the level and the volume each on their own clock
            {
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
                Engine::Snapshot lv, vol;
                lv.fade = vol.fade = 2.0;
                lv.composition = QJsonObject{{"included", true}, {"opacity", 0.0}};
                vol.composition = QJsonObject{{"included", true}, {"volume", 0.0}};
                const int ml = e.addSnapshot(lv), mv = e.addSnapshot(vol);
                e.recallSnapshot(ml);
                e.advanceFades(1.0);
                CHECK(near(e.compositionOpacityTarget(), 0.5));
                e.recallSnapshot(mv); // the volume only: the level goes on
                e.advanceFades(1.0);
                CHECK(near(e.compositionOpacityTarget(), 0.0) && near(e.audioVolume(), 0.5));
                e.advanceFades(2.0);
                e.removeSnapshot(mv);
                e.removeSnapshot(ml);
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
            }

            // A step's action is its snapshot's longest time
            CHECK(near(e.snapshotDuration(e.snapshot(mA1).id), 2.0) && e.snapshotDuration(987654) == 0);
            {
                Engine::Snapshot m = e.snapshot(mB1);
                QJsonArray ls = m.layers;
                for (int k = 0; k < ls.size(); ++k) {
                    QJsonObject o = ls[k].toObject();
                    if (o.value("included").toBool()) o["timing"] = QJsonObject{{"opacity", 5.0}, {"opacity/curve", "linear"}};
                    ls[k] = o;
                }
                m.layers = ls;
                e.setSnapshot(mB1, m);
                CHECK(near(e.snapshotDuration(m.id), 5.0));
                m.layers = e.snapshot(mA1).layers; // back to the snapshot's fade (2 s)
                for (int k = 0; k < m.layers.size(); ++k) {
                    QJsonObject o = m.layers[k].toObject();
                    const bool me = o.value("id").toString().toULongLong() == ib;
                    o["included"] = me;
                    o["opacity"] = 1.0;
                    m.layers[k] = o;
                }
                e.setSnapshot(mB1, m);
            }

            // Pre-wait, post-wait, follow / auto-follow / wait
            std::vector<int> fired;
            e.setRecaller([&](int mi) {
                fired.push_back(mi);
                e.recallSnapshot(mi);
            });
            Engine::Sequence seq;
            seq.name = "Waits";
            Engine::SequenceStep s0, s1, s2;
            s0.snapshot = e.snapshot(mA1).id;
            s0.preWait = 1.0;
            s0.postWait = 0.5;
            s0.next = Engine::StepContinue::Follow; // the next GO 0.5 s after this one is triggered
            s1.snapshot = e.snapshot(mB1).id;
            s1.postWait = 0.5;
            s1.next = Engine::StepContinue::AutoFollow; // the next GO 0.5 s after its 2 s action
            s2.snapshot = e.snapshot(mA0).id;               // Wait
            seq.steps = {s0, s1, s2};
            e.setCurrentSequence(e.addSequence(seq));
            CHECK(e.sequenceGo() && e.sequencePosition() == 0 && fired.empty()); // in its pre-wait
            CHECK(e.sequenceRunning() && e.sequenceRuns().size() == 1 && !e.sequenceRuns()[0].fired);
            e.advanceSequence(0.5);
            CHECK(fired.empty());
            e.advanceSequence(0.5);
            CHECK(fired == std::vector<int>{mA1} && e.sequencePosition() == 0);
            e.advanceSequence(0.4);
            CHECK(e.sequencePosition() == 0);
            e.advanceSequence(0.1); // 1.5 s: pre-wait + post-wait, the next one without waiting for the action
            CHECK(fired == (std::vector<int>{mA1, mB1}) && e.sequencePosition() == 1);
            e.advanceSequence(2.4);
            CHECK(e.sequencePosition() == 1);
            e.advanceSequence(0.2); // its action (2 s) and its post-wait (0.5 s) over
            CHECK(fired == (std::vector<int>{mA1, mB1, mA0}) && e.sequencePosition() == 2);
            e.advanceSequence(10.0);
            CHECK(fired.size() == 3 && !e.sequenceRunning() && e.sequenceRuns().empty()); // Wait: GO is awaited
            // All at once: the time beyond each GO is carried over to the next
            fired.clear();
            e.setSequencePosition(-1);
            e.sequenceGo();
            e.advanceSequence(10.0);
            CHECK(fired == (std::vector<int>{mA1, mB1, mA0}) && e.sequencePosition() == 2);
            // Stopped: what was still to come does not come
            fired.clear();
            e.setSequencePosition(-1);
            e.sequenceGo();
            e.sequenceStop();
            e.advanceSequence(10.0);
            CHECK(fired.empty() && e.sequencePosition() == 0);
            // GO during a pre-wait: the playhead has moved on, the next GO is the next step
            fired.clear();
            e.setSequencePosition(-1);
            e.sequenceGo();
            CHECK(e.sequenceNext() == 1);
            // GO BACK: at once, and the waits running are stopped
            e.setSequencePosition(2);
            CHECK(e.sequenceBack() && e.sequencePosition() == 1 && fired == std::vector<int>{mB1});
            e.advanceSequence(10.0);
            CHECK(fired == std::vector<int>{mB1}); // neither step 0's pre-wait nor step 1's auto-follow
            // Saved with the project
            CHECK(e.saveProject(tmp + "/waits.fulskrin", {}, &err));
            CHECK(e.loadProject(tmp + "/waits.fulskrin", nullptr, &err));
            {
                const Engine::Sequence w = e.sequence(e.currentSequence());
                CHECK(w.steps.size() == 3 && near(w.steps[0].preWait, 1.0) && near(w.steps[0].postWait, 0.5) &&
                      w.steps[0].next == Engine::StepContinue::Follow &&
                      w.steps[1].next == Engine::StepContinue::AutoFollow && w.steps[2].next == Engine::StepContinue::Wait);
            }
            e.setRecaller(nullptr);
            e.removeSequence(e.currentSequence());
            for (int mi : {mA0, mB1, mA1}) e.removeSnapshot(mi); // the last first
            e.removeLayer(e.indexOfId(ia));
            e.removeLayer(e.indexOfId(ib));
            e.setFadesManual(false);
        }
        // Timelines: curves and oscillators on their own clock; the sequences drive their transport
        {
            using A = Engine::AnimAction;
            using L = Engine::AnimLoop;
            e.setFadesManual(true);
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            const quint64 tid = e.layerId(e.addLayer("Anim", V));
            auto val = [&](const QString &p) {
                double v = std::nan("");
                e.animParamValue(tid, p, &v);
                return v;
            };
            // A curve: eased from key to key (linear here), held, then its last value
            Engine::AnimTrack c;
            c.layer = tid;
            c.param = "opacity";
            c.keys = {{0, 0, 0}, {2, 1, 0}, {3, 0.2, Engine::kAnimHold}, {4, 0.8, 0}};
            CHECK(near(c.valueAt(1, 0), 0.5) && near(c.valueAt(3.5, 0), 0.2) && near(c.valueAt(5, 0), 0.8) &&
                  near(c.valueAt(-1, 0), 0));
            CHECK(std::isnan(Engine::AnimTrack().valueAt(0, 0))); // no key: no value
            // An oscillator: on the time played, center ± amplitude
            Engine::AnimTrack o;
            o.oscillator = true;
            o.period = 2;
            o.center = 10;
            o.amplitude = 5;
            CHECK(near(o.valueAt(0, 0.5), 15) && near(o.valueAt(0, 1.0), 10) && near(o.valueAt(0, 1.5), 5));
            o.wave = Engine::AnimWave::Triangle;
            CHECK(near(o.valueAt(0, 0.25), 12.5) && near(o.valueAt(0, 1.5), 5));
            o.wave = Engine::AnimWave::Saw;
            CHECK(near(o.valueAt(0, 0), 5) && near(o.valueAt(0, 1), 10));
            o.wave = Engine::AnimWave::Square;
            CHECK(near(o.valueAt(0, 0.2), 15) && near(o.valueAt(0, 1.2), 5));
            // Passes: loop, ping-pong, a number of them
            Engine::Animation a;
            a.name = "Pulse";
            a.duration = 4;
            a.loop = L::PingPong;
            CHECK(near(a.position(5), 3) && !std::isfinite(a.length()));
            a.loop = L::Loop;
            a.repeat = 2;
            CHECK(near(a.position(5), 1) && near(a.length(), 8) && near(a.position(9), 4));
            a.loop = L::Once;
            CHECK(near(a.position(9), 4) && near(a.length(), 4));
            // Playing: the curve repeats its pattern, the oscillator turns the mapping without a jump
            a.loop = L::Loop;
            a.repeat = 0;
            Engine::AnimTrack rot;
            rot.layer = tid;
            rot.param = "spatial/rotation";
            rot.oscillator = true;
            rot.wave = Engine::AnimWave::Saw;
            rot.period = 4;
            rot.center = 0;
            rot.amplitude = 180;
            a.tracks = {c, rot};
            const int ai = e.addAnimation(a);
            const quint64 aid = e.animation(ai).id;
            CHECK(aid != 0 && e.indexOfAnimation(aid) == ai && e.animationCount() >= 1);
            const QRectF before = e.layer(e.indexOfId(tid))->mapping.bounds();
            e.controlAnimation(aid, A::Play);
            CHECK(near(val("opacity"), 0) && e.animation(ai).state == Engine::AnimState::Playing);
            e.advanceFades(1.0);
            CHECK(near(val("opacity"), 0.5) && near(val("spatial/rotation"), -90));
            e.advanceFades(1.0);
            CHECK(near(val("opacity"), 1.0) && near(val("spatial/rotation"), 0));
            CHECK(near(e.layer(e.indexOfId(tid))->mapping.bounds().width(), before.width())); // turned, not shrunk
            e.advanceFades(2.5); // 4.5 s: the second pass of the pattern
            CHECK(near(val("opacity"), 0.25) && near(val("spatial/rotation"), -135));
            // Paused: its values are left alone
            e.controlAnimation(aid, A::Pause);
            e.layer(e.indexOfId(tid))->opacity = 0.9f;
            e.advanceFades(1.0);
            CHECK(near(val("opacity"), 0.9) && e.animation(ai).state == Engine::AnimState::Paused);
            // Seek: the values at once (the hold), then Play goes on from there
            e.controlAnimation(aid, A::Seek, 3.5);
            CHECK(near(val("opacity"), 0.2) && near(e.animation(ai).clock, 3.5));
            e.controlAnimation(aid, A::Play);
            e.advanceFades(0.25);
            CHECK(near(val("opacity"), 0.2) && near(e.animation(ai).clock, 3.75));
            // Stop: back to the start, the values stay
            e.controlAnimation(aid, A::Stop);
            e.advanceFades(1.0);
            CHECK(e.animation(ai).state == Engine::AnimState::Stopped && e.animation(ai).clock == 0 && near(val("opacity"), 0.2));
            // Twice, then over with its last values
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 2);
            e.controlAnimation(aid, A::Play);
            e.advanceFades(9.0);
            CHECK(e.animation(ai).state == Engine::AnimState::Stopped && near(val("opacity"), 0.8));
            // The composition's level
            {
                Engine::Animation lv;
                Engine::AnimTrack t;
                t.param = "opacity";
                t.keys = {{0, 0.3, 0}};
                lv.tracks = {t};
                const quint64 lid = e.animation(e.addAnimation(lv)).id;
                e.controlAnimation(lid, A::Seek, 0);
                CHECK(near(e.compositionOpacityTarget(), 0.3));
                e.removeAnimation(e.indexOfAnimation(lid));
                e.fadeCompositionOpacity(1.0, 0);
            }
            CHECK(!e.animatableParams(tid).empty() && e.animatableParams(0).size() == 3);
            // Steps driving it: Play (its time, when it ends), Seek, Stop
            e.controlAnimation(aid, A::LoopMode, 0, L::Once);
            Engine::Sequence sq;
            sq.name = "Timeline";
            Engine::SequenceStep p0, p1, p2;
            p0.timeline = aid;
            p0.next = Engine::StepContinue::AutoFollow;
            p1.timeline = aid;
            p1.action = A::Seek;
            p1.seekTime = 1.0;
            p2.timeline = aid;
            p2.action = A::Stop;
            Engine::SequenceStep p3;
            p3.timeline = aid;
            p3.action = A::Speed;
            p3.speed = 0.75;
            sq.steps = {p0, p1, p2, p3};
            e.setCurrentSequence(e.addSequence(sq));
            CHECK(near(e.stepDuration(p0), 4) && e.stepDuration(p1) == 0);
            CHECK(e.sequenceGo() && e.animation(ai).state == Engine::AnimState::Playing);
            e.advanceFades(3.9);
            e.advanceSequence(3.9);
            CHECK(e.sequencePosition() == 0);
            e.advanceFades(0.2); // (the timeline and the sequence run together)
            e.advanceSequence(0.2); // its 4 s over: the seek
            CHECK(e.sequencePosition() == 1 && near(e.animation(ai).clock, 1.0) &&
                  e.animation(ai).state == Engine::AnimState::Paused); // over, then sought: stays where it was sought
            CHECK(e.sequenceGo() && e.animation(ai).state == Engine::AnimState::Stopped);
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 0);
            CHECK(e.stepDuration(p0) == 0); // endless: nothing to wait for
            using S = Engine::AnimState;
            // Loop → Once while it plays past its first pass: it ends the pass it is in, no jump to the end
            e.controlAnimation(aid, A::Play);
            e.advanceFades(9.0); // the third pass, 1 s in
            CHECK(near(e.animation(ai).position(), 1.0));
            e.controlAnimation(aid, A::LoopMode, 0, L::Once);
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 1.0));
            e.advanceFades(2.0);
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 3.0));
            e.advanceFades(1.5);
            CHECK(e.animation(ai).state == S::Stopped);
            // Ping-pong on its way back, then Once: back to the start, then over
            e.controlAnimation(aid, A::LoopMode, 0, L::PingPong, 0);
            e.controlAnimation(aid, A::Play);
            e.advanceFades(5.0);
            CHECK(near(e.animation(ai).position(), 3.0));
            e.controlAnimation(aid, A::LoopMode, 0, L::Once);
            e.advanceFades(1.0);
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 2.0));
            e.advanceFades(2.5);
            CHECK(e.animation(ai).state == S::Stopped);
            // The same from the timeline window (an edit of the whole timeline)
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 0);
            e.controlAnimation(aid, A::Play);
            e.advanceFades(6.0);
            {
                Engine::Animation x = e.animation(ai);
                x.loop = L::Once;
                e.setAnimation(ai, x);
            }
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 2.0));
            e.controlAnimation(aid, A::Stop);
            // Speed: twice as fast; a step waits for the real time left
            {
                Engine::Animation x = e.animation(ai);
                x.speed = 2;
                e.setAnimation(ai, x);
            }
            e.controlAnimation(aid, A::Play);
            e.advanceFades(1.0);
            CHECK(near(e.animation(ai).clock, 2.0) && near(e.stepDuration(p0), 1.0));
            e.controlAnimation(aid, A::Stop);
            // Speed from a step (or OSC): it plays on from where it is
            e.controlAnimation(aid, A::Play);
            e.advanceFades(1.0);
            e.controlAnimation(aid, A::Speed, 0.5);
            e.advanceFades(1.0);
            CHECK(near(e.animation(ai).speed, 0.5) && near(e.animation(ai).clock, 2.5));
            e.controlAnimation(aid, A::Speed, 0); // frozen where it is, still playing
            e.advanceFades(1.0);
            CHECK(near(e.animation(ai).clock, 2.5) && e.animation(ai).state == Engine::AnimState::Playing &&
                  !std::isfinite(e.stepDuration(p0)));
            e.controlAnimation(aid, A::Stop);
            // An auto-follow Play on a frozen timeline waits; its speed raised meanwhile (OSC), it goes on, and the
            // next step comes once the timeline is really over
            {
                const int keepSeq = e.currentSequence();
                Engine::Animation x = e.animation(ai);
                x.loop = L::Once;
                x.speed = 0;
                e.setAnimation(ai, x);
                Engine::Sequence fq;
                fq.name = "Frozen";
                Engine::SequenceStep f0, f1;
                f0.timeline = aid;
                f0.next = Engine::StepContinue::AutoFollow;
                f1.timeline = aid;
                f1.action = A::Pause;
                fq.steps = {f0, f1};
                const int fi = e.addSequence(fq);
                e.setCurrentSequence(fi);
                CHECK(!std::isfinite(e.stepDuration(f0)));
                CHECK(e.sequenceGo() && e.animation(ai).state == Engine::AnimState::Playing);
                for (int k = 0; k < 3; ++k) {
                    e.advanceFades(10.0);
                    e.advanceSequence(10.0);
                }
                CHECK(e.sequencePosition() == 0 && near(e.animation(ai).clock, 0)); // still waiting
                e.controlAnimation(aid, A::Speed, 2);
                e.advanceSequence(0.01); // it sees the speed
                e.advanceFades(1.0);
                e.advanceSequence(1.0);
                CHECK(e.sequencePosition() == 0 && near(e.animation(ai).clock, 2.0));
                e.controlAnimation(aid, A::Speed, 0); // frozen again: it waits again
                e.advanceFades(5.0);
                e.advanceSequence(5.0);
                CHECK(e.sequencePosition() == 0 && near(e.animation(ai).clock, 2.0));
                e.controlAnimation(aid, A::Speed, 2);
                e.advanceSequence(0.01);
                e.advanceFades(1.5);
                e.advanceSequence(1.5); // its 4 s played: over, and the next step comes
                CHECK(e.sequencePosition() == 1);
                e.sequenceStop();
                e.setCurrentSequence(keepSeq);
                e.removeSequence(fi);
                e.controlAnimation(aid, A::Stop);
                x.speed = 2;
                e.setAnimation(ai, x);
            }
            // Undo: the arrows of a number merge into one step
            {
                QUndoStack st;
                const Engine::Animation b = e.animation(ai);
                Engine::Animation a1 = b, a2 = b;
                a1.speed = 1.5;
                a2.speed = 1;
                st.push(new cmd::SetAnimation(&e, aid, b, a1, "Speed", "speed"));
                st.push(new cmd::SetAnimation(&e, aid, a1, a2, "Speed", "speed"));
                CHECK(st.count() == 1 && near(e.animation(ai).speed, 1));
                st.undo();
                CHECK(near(e.animation(ai).speed, 2));
                st.redo();
                auto *add = new cmd::AddAnimation(&e, Engine::Animation(), e.animationCount(), "New");
                st.push(add);
                const quint64 nid = add->animationId();
                CHECK(nid != 0 && e.indexOfAnimation(nid) >= 0);
                st.push(new cmd::RemoveAnimation(&e, e.indexOfAnimation(nid)));
                CHECK(e.indexOfAnimation(nid) < 0);
                st.undo();
                CHECK(e.indexOfAnimation(nid) >= 0);
                st.undo();
                CHECK(e.indexOfAnimation(nid) < 0);
            }
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 0);
            // A first key "current value": the number starts from where it is, no jump
            quint64 cid = 0;
            {
                e.layer(e.indexOfId(tid))->opacity = 0.6f;
                Engine::Animation cv;
                cv.name = "From here";
                cv.duration = 2;
                cv.loop = L::Once;
                Engine::AnimTrack t;
                t.layer = tid;
                t.param = "opacity";
                t.keys = {{0, 0, 0}, {2, 1, 0}};
                t.keys[0].isCurrentValue = true;
                cv.tracks = {t};
                cid = e.animation(e.addAnimation(cv)).id;
                e.controlAnimation(cid, A::Play);
                CHECK(near(val("opacity"), 0.6));
                e.advanceFades(1.0);
                CHECK(near(val("opacity"), 0.8));
                e.advanceFades(1.5);
                CHECK(near(val("opacity"), 1.0));
                e.layer(e.indexOfId(tid))->opacity = 0.2f;
                e.controlAnimation(cid, A::Play); // from 0.2 now
                e.advanceFades(1.0);
                CHECK(near(val("opacity"), 0.6));
                e.controlAnimation(cid, A::Stop);
            }
            // Saved with the project
            CHECK(e.saveProject(tmp + "/timelines.fulskrin", {}, &err));
            CHECK(e.loadProject(tmp + "/timelines.fulskrin", nullptr, &err));
            {
                const int k = e.indexOfAnimation(aid);
                CHECK(k >= 0);
                const Engine::Animation x = e.animation(k);
                CHECK(x.name == "Pulse" && near(x.duration, 4) && x.loop == L::Loop && x.tracks.size() == 2 &&
                      x.tracks[0].keys.size() == 4 && x.tracks[0].keys[2].curve == Engine::kAnimHold &&
                      x.tracks[0].keys[1].curve == 0 && x.tracks[1].oscillator &&
                      x.tracks[1].wave == Engine::AnimWave::Saw && near(x.tracks[1].amplitude, 180) &&
                      x.tracks[0].layer == tid && x.state == Engine::AnimState::Stopped);
                const int ck = e.indexOfAnimation(cid);
                CHECK(ck >= 0 && e.animation(ck).tracks[0].keys[0].isCurrentValue && !e.animation(ck).tracks[0].keys[1].isCurrentValue &&
                      e.animation(ck).tracks[0].keys[0].curve == 0);
                if (ck >= 0) e.removeAnimation(ck);
                const Engine::Sequence w = e.sequence(e.currentSequence());
                CHECK(w.steps.size() == 4 && w.steps[3].action == A::Speed && near(w.steps[3].speed, 0.75) &&
                      w.steps[0].timeline == aid && w.steps[0].snapshot == 0 &&
                      w.steps[0].action == A::Play && w.steps[1].action == A::Seek && near(w.steps[1].seekTime, 1.0) &&
                      w.steps[2].action == A::Stop);
            }
            e.removeSequence(e.currentSequence());
            e.removeAnimation(e.indexOfAnimation(aid));
            e.removeLayer(e.indexOfId(tid));
            e.setFadesManual(false);
        }
        // A recall shows what was stored: a layer the snapshot does not know fades out and is hidden
        {
            const int xi = e.addLayer("Extra", V);
            const quint64 xid = e.layerId(xi);
            e.layer(xi)->opacity = 0.7f;
            Engine::Snapshot m = e.snapshot(0);
            const double keptFade = m.fade;
            m.fade = 0.5;
            e.setSnapshot(0, m);
            e.setFadesManual(true);
            e.recallSnapshot(0);
            CHECK(e.layer(e.indexOfId(xid))->enabled);
            e.advanceFades(0.25);
            CHECK(e.layer(e.indexOfId(xid))->opacity < 0.7f && e.layer(e.indexOfId(xid))->opacity > 0.0f);
            e.advanceFades(0.3);
            CHECK(!e.layer(e.indexOfId(xid))->enabled && std::abs(e.layer(e.indexOfId(xid))->opacity - 0.7f) < 1e-6);
            // Applying states (undo of a recall) leaves the others alone
            e.layer(e.indexOfId(xid))->enabled = true;
            e.applyLayers(e.snapshot(0).layers, 0);
            CHECK(e.layer(e.indexOfId(xid))->enabled);
            e.setFadesManual(false);
            e.removeLayer(e.indexOfId(xid));
            m.fade = keptFade;
            e.setSnapshot(0, m);
        }

        // The inspector edits what a snapshot holds (its layers' JSON): the recall then applies the new values,
        // and the composition does not move until it is recalled.
        {
            Engine::Snapshot edited = e.snapshot(0);
            QJsonObject l0 = edited.layers[0].toObject();
            l0["opacity"] = 0.25;
            l0["blend_mode"] = "screen";
            QJsonObject l1 = edited.layers[1].toObject();
            QJsonObject src = l1.value("source").toObject();
            QJsonObject params = src.value("params").toObject();
            params["color"] = QJsonArray{0.25, 0.5, 0.75, 1.0}; // SolidColor
            src["params"] = params;
            l1["source"] = src;
            l1["enable"] = true;
            edited.layers[0] = l0;
            edited.layers[1] = l1;
            e.layer(V + 0)->opacity = 0.8f;
            e.setSnapshot(0, edited);
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6); // editing a snapshot does not touch the layers
            CHECK(std::abs(e.snapshot(0).layers[0].toObject().value("opacity").toDouble() - 0.25) < 1e-9);
            e.recallSnapshot(0);
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.25f) < 1e-6 && e.layer(V + 0)->blend == BlendMode::Screen);
            CHECK(e.layer(V + 1)->enabled);
            const IsfInstance *gen = e.layer(V + 1)->generator.get();
            double r = -1;
            for (const IsfInput &in : gen->inputs())
                if (in.name == "color") r = in.cValue[0];
            CHECK(std::abs(r - 0.25) < 1e-6);
            // Still edited after a save / load round trip
            CHECK(e.saveProject(tmp + "/snapshots.fulskrin", {}, &err));
            CHECK(e.loadProject(tmp + "/snapshots.fulskrin", nullptr, &err));
            CHECK(std::abs(e.snapshot(0).layers[0].toObject().value("opacity").toDouble() - 0.25) < 1e-9);
        }
        e.newProject();
        CHECK(e.snapshotCount() == 0);
    }

    // 4f. Effect masks: an effect applies where another layer's picture is white
    {
        QFile inv(tmp + "/Invert.fs");
        inv.open(QIODevice::WriteOnly);
        inv.write("/*{ \"ISFVSN\": \"2\", \"INPUTS\": [ { \"NAME\": \"inputImage\", \"TYPE\": \"image\" } ] }*/\n"
                  "void main() { vec4 c = IMG_THIS_PIXEL(inputImage); gl_FragColor = vec4(1.0 - c.rgb, c.a); }\n");
        inv.close();
        QImage red(32, 16, QImage::Format_RGB32), half(32, 16, QImage::Format_RGB32), grey(32, 16, QImage::Format_RGB32);
        red.fill(qRgb(255, 0, 0));
        grey.fill(qRgb(128, 128, 128));
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 32; ++x) half.setPixel(x, y, x < 16 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        red.save(tmp + "/mred.png");
        half.save(tmp + "/mhalf.png");
        grey.save(tmp + "/mgrey.png");
        const int mi = e.addLayer("MaskSrc", V);
        CHECK(e.setLayerImage(mi, tmp + "/mhalf.png", &err));
        e.layer(mi)->enabled = false; // a hidden layer still masks
        const quint64 maskId = e.layerId(mi);
        const int li = e.addLayer("Masked", V);
        CHECK(e.setLayerImage(li, tmp + "/mred.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 lid = e.layerId(li);
        CHECK(e.addEffect(li, tmp + "/Invert.fs") == 0);
        auto at = [&](double fx) {
            e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            return g.pixelColor(int(g.width() * fx), g.height() / 2);
        };
        CHECK(at(0.25).red() < 5 && at(0.75).red() < 5); // no mask: everywhere
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, maskId, false, &err));
        QColor l = at(0.25), r = at(0.75);
        CHECK(l.red() < 5 && l.green() > 250 && r.red() > 250 && r.green() < 5); // white: the effect, black: the input
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, maskId, true, &err));
        l = at(0.25), r = at(0.75);
        CHECK(l.red() > 250 && r.red() < 5); // inverted
        CHECK(e.layerJson(e.indexOfId(lid)).value("fx").toArray().at(0).toObject().value("mask").toString() ==
              QString::number(maskId));
        // Mask picture before or after the mask layer's effects (no effect on the mask here: same picture)
        CHECK(e.setEffectMaskTap(e.indexOfId(lid), 0, true));
        CHECK(e.layerJson(e.indexOfId(lid)).value("fx").toArray().at(0).toObject().value("mask_tap").toString() == "prefx");
        l = at(0.25);
        CHECK(l.red() > 250 && at(0.75).red() < 5);
        CHECK(e.setEffectMaskTap(e.indexOfId(lid), 0, false));
        // Grey: in proportion
        CHECK(e.setLayerImage(e.indexOfId(maskId), tmp + "/mgrey.png", &err));
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, maskId, false, &err));
        l = at(0.5);
        CHECK(l.red() > 100 && l.red() < 160 && l.green() > 100 && l.green() < 160);
        // Refused: itself, a viewport, a layer that already depends on this one
        CHECK(!e.setEffectMask(e.indexOfId(lid), 0, lid, false));
        CHECK(!e.setEffectMask(e.indexOfId(lid), 0, e.mainViewportId(), false));
        CHECK(e.layerDependsOn(lid, maskId)); // a mask is a dependency: the mask layer cannot take this one as source
        CHECK(!e.setLayerSourceLayer(e.indexOfId(maskId), lid, LayerTap::PostFx, &err));
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, 0, false, &err));
        CHECK(e.setLayerSourceLayer(e.indexOfId(maskId), lid, LayerTap::PostFx, &err));
        err.clear();
        CHECK(!e.setEffectMask(e.indexOfId(lid), 0, maskId, false, &err) && err.contains("feed back"));
        // The mask layer gone: the effect applies everywhere again
        e.removeLayer(e.indexOfId(maskId));
        CHECK(at(0.25).red() < 5 && at(0.75).red() < 5);
        e.removeLayer(e.indexOfId(lid));
    }
    // 4f bis. The color section through a mask: red with all its red removed, only where the mask is white
    {
        const int mi = e.addLayer("ColorMaskSrc", V);
        CHECK(e.setLayerImage(mi, tmp + "/mhalf.png", &err));
        e.layer(mi)->enabled = false;
        const quint64 maskId = e.layerId(mi);
        const int li = e.addLayer("Colored", V);
        CHECK(e.setLayerImage(li, tmp + "/mred.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 lid = e.layerId(li);
        e.layer(li)->color.remove[0] = 1.0f; // red filtered out: black
        e.layer(li)->color.add[2] = 1.0f;    // blue light added
        auto at = [&](double fx) {
            e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            return g.pixelColor(int(g.width() * fx), g.height() / 2);
        };
        QColor l = at(0.25), r = at(0.75);
        CHECK(l.red() < 5 && l.blue() > 250 && r.red() < 5 && r.blue() > 250); // no mask: everywhere
        CHECK(e.setColorMask(e.indexOfId(lid), maskId, false, &err));
        l = at(0.25), r = at(0.75);
        CHECK(l.red() < 5 && l.blue() > 250 && r.red() > 250 && r.blue() < 5); // white: colored, black: as it was
        CHECK(e.setColorMask(e.indexOfId(lid), maskId, true, &err));
        l = at(0.25), r = at(0.75);
        CHECK(l.red() > 250 && l.blue() < 5 && r.red() < 5 && r.blue() > 250); // inverted
        // The section switched off: no color, mask or not
        e.layer(e.indexOfId(lid))->color.enabled = false;
        l = at(0.25), r = at(0.75);
        CHECK(l.red() > 250 && r.red() > 250);
        e.layer(e.indexOfId(lid))->color.enabled = true;
        // Saved, read back, recalled by a snapshot
        const QJsonObject json = e.layerJson(e.indexOfId(lid));
        CHECK(json.value("color").toObject().value("mask").toString() == QString::number(maskId)
              && json.value("color").toObject().value("mask_invert").toBool());
        CHECK(e.setColorMask(e.indexOfId(lid), 0, false, &err));
        e.replaceLayerJson(e.indexOfId(lid), json);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *x = e.layer(e.indexOfId(lid));
            CHECK(x && x->color.maskLayer == maskId && x->color.maskInvert);
        }
        // Refused: itself, a viewport, a feedback; a mask counts as a dependency
        CHECK(!e.setColorMask(e.indexOfId(lid), lid, false));
        CHECK(!e.setColorMask(e.indexOfId(lid), e.mainViewportId(), false));
        CHECK(e.layerDependsOn(lid, maskId));
        CHECK(!e.setLayerSourceLayer(e.indexOfId(maskId), lid, LayerTap::PostFx, &err));
        // The mask layer gone: the color applies everywhere again, the reference dropped on reading
        e.removeLayer(e.indexOfId(maskId));
        l = at(0.25), r = at(0.75);
        CHECK(l.blue() > 250 && r.blue() > 250);
        e.fixLayerReferences();
        CHECK(e.layer(e.indexOfId(lid))->color.maskLayer == 0);
        e.removeLayer(e.indexOfId(lid));
    }

    // 4d''. Soft edge: the picture fades out towards the sides it is set on
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(40, 40));
        QImage white(40, 40, QImage::Format_RGB32);
        white.fill(qRgb(255, 255, 255));
        white.save(tmp + "/se_white.png");
        const int li = e.addLayer("Soft");
        const quint64 id = e.layerId(li);
        CHECK(e.setLayerImage(li, tmp + "/se_white.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        CHECK(e.grabOutput().pixelColor(0, 20).red() > 250); // off: untouched
        {
            Engine::Lock lk(&e.mutex());
            SoftEdge &se = e.layer(li)->mapping.soft;
            se.enabled = true;
            se.width[SoftEdge::Left] = 0.5; // fades in from the left side only
            se.width[SoftEdge::Right] = se.width[SoftEdge::Top] = se.width[SoftEdge::Bottom] = 0;
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        QImage g = e.grabOutput();
        CHECK(g.pixelColor(1, 20).red() < 40);                                      // dark at the left side
        CHECK(std::abs(g.pixelColor(10, 20).red() - 134) < 24);                     // about half way: 10.5 px of 20
        CHECK(g.pixelColor(30, 20).red() > 250 && g.pixelColor(30, 1).red() > 250); // nothing elsewhere
        {
            Engine::Lock lk(&e.mutex());
            SoftEdge &se = e.layer(li)->mapping.soft;
            se.width[SoftEdge::Left] = 0;
            se.width[SoftEdge::Top] = 0.5; // the top of the picture is the top of the output
            se.power[SoftEdge::Top] = 2;
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        g = e.grabOutput();
        CHECK(g.pixelColor(20, 1).red() < 40 && g.pixelColor(20, 38).red() > 250);
        CHECK(std::abs(g.pixelColor(20, 10).red() - 70) < 24); // (10.5/20)^2 of 255
        // The picture's own top: a red top half is at the top of the output, and the fade follows it
        QImage two(40, 40, QImage::Format_RGB32);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) two.setPixel(x, y, y < 20 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        two.save(tmp + "/se_two.png");
        CHECK(e.setLayerImage(e.indexOfId(id), tmp + "/se_two.png", &err));
        for (int k = 0; k < 2; ++k) e.renderFrame();
        g = e.grabOutput();
        CHECK(g.pixelColor(20, 19).red() > 100 && g.pixelColor(20, 35).blue() > 250);
        // Saved and loaded
        Mapping m;
        m.fromJson(e.layerJson(e.indexOfId(id)).value("spatial").toObject());
        CHECK(m.soft.enabled && m.soft.width[SoftEdge::Top] == 0.5 && m.soft.power[SoftEdge::Top] == 2);
    }

    // 4d'. Blend modes that take away: Subtract and Difference (against what is drawn below)
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(16, 16));
        QImage up(16, 16, QImage::Format_RGB32), down(16, 16, QImage::Format_RGB32);
        up.fill(qRgb(50, 200, 20));
        down.fill(qRgb(200, 120, 50));
        up.save(tmp + "/bl_up.png");
        down.save(tmp + "/bl_down.png");
        const int low = e.addLayer("Bottom");
        const quint64 bottomId = e.layerId(low);
        const int high = e.addLayer("Top"); // new layers go to the top of the list
        const quint64 topId = e.layerId(high);
        CHECK(e.setLayerImage(e.indexOfId(topId), tmp + "/bl_up.png", &err));
        CHECK(e.setLayerImage(e.indexOfId(bottomId), tmp + "/bl_down.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(topId))->mapping.resetCorners();
            e.layer(e.indexOfId(bottomId))->mapping.resetCorners();
        }
        auto grab = [&](BlendMode m) {
            {
                Engine::Lock lk(&e.mutex());
                e.layer(e.indexOfId(topId))->blend = m;
            }
            for (int k = 0; k < 2; ++k) e.renderFrame();
            return e.grabOutput().pixelColor(8, 8);
        };
        auto near = [](const QColor &c, int r, int g, int b) {
            return std::abs(c.red() - r) <= 3 && std::abs(c.green() - g) <= 3 && std::abs(c.blue() - b) <= 3;
        };
        CHECK(near(grab(BlendMode::Subtract), 150, 0, 30));   // below minus above, never under zero
        CHECK(near(grab(BlendMode::Difference), 150, 80, 30)); // |below - above|
        CHECK(near(grab(BlendMode::Normal), 50, 200, 20));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(topId))->opacity = 0.5f;
        }
        CHECK(near(grab(BlendMode::Difference), 175, 20, 40)); // the layer at half strength (its color halfway to black)
        CHECK(blendModeFromKey(blendModeKey(BlendMode::Subtract)) == BlendMode::Subtract &&
              blendModeFromKey(blendModeKey(BlendMode::Difference)) == BlendMode::Difference);
    }

    // 4d-bis. Color depth: the render targets follow the project's setting
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(16, 16));
        QImage col(16, 16, QImage::Format_RGB32);
        col.fill(qRgb(200, 100, 50));
        col.save(tmp + "/dp_col.png");
        const int li = e.addLayer("Depth");
        CHECK(e.setLayerImage(li, tmp + "/dp_col.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
            e.layer(li)->opacity = 0.5f;
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        const QColor c8 = e.grabOutput().pixelColor(8, 8);
        CHECK(renderBits() == 8);
        Engine::RenderSettings rs = e.renderSettings();
        rs.depth = 10;
        e.setRenderSettings(rs);
        for (int k = 0; k < 3; ++k) e.renderFrame();
        const QColor c10 = e.grabOutput().pixelColor(8, 8);
        CHECK(renderBits() == 16 && e.effectiveRender().depth == 10);
        CHECK(std::abs(c10.red() - c8.red()) <= 1 && std::abs(c10.green() - c8.green()) <= 1 && std::abs(c10.blue() - c8.blue()) <= 1);
        CHECK(std::abs(c10.red() - 100) <= 1 && std::abs(c10.green() - 50) <= 1); // half of (200, 100, 50)
        // Rendering is the machine's (Settings): a project does not keep it
        const QString p = tmp + "/depth.fulskrin";
        CHECK(e.saveProject(p, QJsonObject(), &err));
        e.newProject();
        CHECK(e.renderSettings().depth == -1);
        CHECK(e.loadProject(p, nullptr, &err));
        CHECK(e.renderSettings().depth == -1 && !readJson(p).contains("render"));
        Engine::RenderSettings d = e.renderDefaults();
        d.depth = 10;
        e.setRenderDefaults(d);
        e.newProject();
        CHECK(e.renderSettings().depth == -1 && e.effectiveRender().depth == 10);
        d.depth = 8;
        e.setRenderDefaults(d);
        for (int k = 0; k < 2; ++k) e.renderFrame();
        CHECK(renderBits() == 8);
    }

    // 4e. Viewports: windows onto one composition, routing, groups inside groups
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 16));
        const int v1 = e.viewports().first();
        e.setViewportSize(v1, QSize(32, 16));
        {
            Engine::Lock lk(&e.mutex());
            Mapping &m = e.layer(v1)->mapping; // left half of the composition
            m.setCorner(0, QPointF(0, 0)), m.setCorner(1, QPointF(0.5, 0)), m.setCorner(2, QPointF(0.5, 1)),
                m.setCorner(3, QPointF(0, 1));
        }
        const int v2i = e.addViewport(QStringLiteral("Right"), QSize(32, 16));
        const quint64 vp1 = e.layerId(e.viewports().first()), vp2 = e.layerId(v2i);
        {
            Engine::Lock lk(&e.mutex());
            const QRectF r = e.layer(e.indexOfId(vp2))->mapping.bounds(); // placed to the right of the first
            CHECK(std::abs(r.left() - 0.5) < 1e-9 && std::abs(r.width() - 0.5) < 1e-9);
        }
        CHECK(e.viewports().size() == 2 && e.viewports()[0] == 0 && e.viewports()[1] == 1); // they come first
        // One layer over the whole composition: red on the left half, blue on the right half
        QImage wide(64, 16, QImage::Format_RGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 64; ++x) wide.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        wide.save(tmp + "/wide.png");
        const int li = e.addLayer("Wide");
        CHECK(li == 2 && e.setLayerImage(li, tmp + "/wide.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
        }
        for (int k = 0; k < 3; ++k) e.renderFrame();
        QImage a = e.grabViewport(vp1), b = e.grabViewport(vp2);
        CHECK(a.size() == QSize(32, 16) && b.size() == QSize(32, 16));
        CHECK(a.pixelColor(16, 8).red() > 250 && a.pixelColor(16, 8).blue() < 5); // each sees its part
        CHECK(b.pixelColor(16, 8).blue() > 250 && b.pixelColor(16, 8).red() < 5);
        // Each viewport in a window of its own: both windows get their picture (each one has its own context)
        {
            QWindow w1, w2;
            int x = 100;
            for (QWindow *w : {&w1, &w2}) { // side by side: a window under another one would be grabbed as it
                w->setSurfaceType(QSurface::OpenGLSurface);
                w->setFormat(QSurfaceFormat::defaultFormat());
                w->setGeometry(x, 100, 64, 32);
                x += 200;
                w->create();
                w->show();
            }
            QElapsedTimer t;
            t.start();
            while ((!w1.isExposed() || !w2.isExposed()) && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            e.setViewportWindow(vp1, &w1);
            e.setViewportWindow(vp2, &w2);
            e.setViewportExposed(vp1, true, w1.size() * w1.devicePixelRatio());
            e.setViewportExposed(vp2, true, w2.size() * w2.devicePixelRatio());
            for (int k = 0; k < 3; ++k) {
                e.renderFrame();
                QCoreApplication::processEvents();
            }
            const QImage g1 = w1.screen()->grabWindow(w1.winId()).toImage(), g2 = w2.screen()->grabWindow(w2.winId()).toImage();
            CHECK(!g1.isNull() && !g2.isNull());
            if (!g1.isNull() && !g2.isNull()) {
                const QColor c1 = g1.pixelColor(g1.width() / 2, g1.height() / 2), c2 = g2.pixelColor(g2.width() / 2, g2.height() / 2);
                CHECK(c1.red() > 200 && c1.blue() < 50);
                CHECK(c2.blue() > 200 && c2.red() < 50);
            }
            e.setViewportWindow(vp1, nullptr);
            e.setViewportWindow(vp2, nullptr);
        }
        // Publishing follows a viewport changed without going through setPublishSettings (undo, snapshots, load)
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->vpPublish[PublishKind::Syphon].enabled = true;
        }
        e.renderFrame();
        CHECK(e.publishState(vp2, PublishKind::Syphon).level != PublishState::Off);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->vpPublish[PublishKind::Syphon].enabled = false;
        }
        e.renderFrame();
        CHECK(e.publishState(vp2, PublishKind::Syphon).level == PublishState::Off);
        // Left out of the second viewport: the first still shows it
        e.setOpacityIn(li, vp2, 0.0f);
        for (int k = 0; k < 2; ++k) e.renderFrame();
        a = e.grabViewport(vp1);
        b = e.grabViewport(vp2);
        CHECK(a.pixelColor(16, 8).red() > 250 && b.pixelColor(16, 8).blue() < 5 && b.pixelColor(16, 8).red() < 5);
        CHECK(e.layerJson(li).value("viewports").toObject().size() == 1);
        // Half of it in the second viewport: the blue is halfway to black, the first viewport is unchanged
        e.setOpacityIn(li, vp2, 0.5f);
        for (int k = 0; k < 2; ++k) e.renderFrame();
        a = e.grabViewport(vp1);
        b = e.grabViewport(vp2);
        CHECK(std::abs(b.pixelColor(16, 8).blue() - 128) < 4 && a.pixelColor(16, 8).red() > 250);
        e.setOpacityIn(li, vp2, 1.0f);
        CHECK(e.layerJson(li).value("viewports").toObject().isEmpty());
        // A viewport has its own color and opacity
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->opacity = 0.5f;
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        b = e.grabViewport(vp2);
        CHECK(std::abs(b.pixelColor(16, 8).blue() - 128) < 8);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->opacity = 1.0f;
        }
        // Groups inside groups: the layer goes into an inner group, held by an outer group
        const quint64 lid = e.layerId(li);
        const quint64 outer = e.layerId(e.addGroup("Outer", e.layerCount()));
        const quint64 inner = e.layerId(e.addGroup("Inner", e.layerCount()));
        e.setStructure(tree::intoGroup(e.structure(), {inner}, outer));
        e.setStructure(tree::intoGroup(e.structure(), {lid}, inner));
        CHECK(e.layer(e.indexOfId(lid))->parent == inner && e.layer(e.indexOfId(inner))->parent == outer);
        CHECK(tree::depthOf(e.structure(), lid) == 2);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(outer))->opacity = 0.5f;
        }
        for (int k = 0; k < 3; ++k) e.renderFrame();
        a = e.grabViewport(vp1);
        CHECK(std::abs(a.pixelColor(16, 8).red() - 128) < 8); // through both groups, at the outer's opacity
        // Routing belongs to the top: a layer inside a group cannot be routed on its own
        e.setOpacityIn(e.indexOfId(lid), vp1, 0.0f);
        CHECK(e.layer(e.indexOfId(lid))->viewportOpacity.empty());
        // Removing a group: its contents go up one level only
        e.removeLayer(e.indexOfId(inner));
        CHECK(e.layer(e.indexOfId(lid))->parent == outer);
        // Snapshots leave the viewports alone, but recall the routing of the layers
        e.setOpacityIn(e.indexOfId(outer), vp2, 0.0f);
        Engine::Snapshot mem;
        mem.layers = e.captureLayers();
        bool hasViewport = false;
        for (const QJsonValue &v : mem.layers) hasViewport |= v.toObject().value("viewport").toBool();
        CHECK(!hasViewport);
        e.setOpacityIn(e.indexOfId(outer), vp2, 1.0f);
        e.applyLayers(mem.layers, 0);
        CHECK(!e.layer(e.indexOfId(outer))->shownIn(vp2));
        // Saved and read back: sizes, places, screens, routing
        e.setViewportOutput(e.indexOfId(vp2), QStringLiteral("HDMI-1"), 2);
        CHECK(e.saveProject(tmp + "/viewports.fulskrin", {}, &err));
        const QJsonObject saved = readJson(tmp + "/viewports.fulskrin");
        CHECK(e.loadProject(tmp + "/viewports.fulskrin", nullptr, &err));
        CHECK(e.viewports().size() == 2);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *v = e.layer(e.indexOfId(vp2));
            CHECK(v && v->isViewport && v->viewportSize() == QSize(32, 16) && v->vpScreen == "HDMI-1" && v->vpMode == 2);
            CHECK(std::abs(v->mapping.bounds().left() - 0.5) < 1e-9);
            CHECK(!e.layer(e.indexOfId(outer))->shownIn(vp2));
        }
        CHECK(e.saveProject(tmp + "/viewports2.fulskrin", {}, &err));
        CHECK(saved.value("layers") == readJson(tmp + "/viewports2.fulskrin").value("layers"));
        // A hidden outer group silences what is inside its inner groups too
        {
            const quint64 in2 = e.layerId(e.addGroup("Inner 2", e.layerCount()));
            e.setStructure(tree::intoGroup(e.structure(), {in2}, outer));
            e.setStructure(tree::intoGroup(e.structure(), {lid}, in2));
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(outer))->enabled = false;
        }
        e.renderFrame();
        CHECK(!e.layer(e.indexOfId(lid))->parentEnabled);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(outer))->enabled = true;
        }
        e.renderFrame();
        CHECK(e.layer(e.indexOfId(lid))->parentEnabled);
        // Undoing the deletion of the first viewport puts it back first
        {
            const QJsonObject first = e.layerJson(0);
            e.removeLayer(0);
            CHECK(e.layerId(0) == vp2);
            e.insertLayerJson(0, first);
            CHECK(e.layerId(0) == vp1 && e.layerId(1) == vp2);
        }
        // A viewport can go as long as another one stays
        e.removeLayer(e.indexOfId(vp2));
        CHECK(e.viewports().size() == 1);
        e.removeLayer(e.viewports().first());
        CHECK(e.viewports().size() == 1);
        // A new project shows its composition through one viewport
        e.newProject();
        CHECK(e.viewports().size() == 1 && e.layerCount() == 1);
    }

    // 4g. Antialiasing: multisampled edges, mipmaps for pictures drawn smaller; the project's choice or the default
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 64));
        e.setViewportSize(e.viewports().first(), QSize(64, 64));
        QImage white(64, 64, QImage::Format_RGB32), checker(64, 64, QImage::Format_RGB32);
        white.fill(Qt::white);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) checker.setPixel(x, y, (x + y) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        white.save(tmp + "/aawhite.png");
        checker.save(tmp + "/aachecker.png");
        const int li = e.addLayer("Edge");
        CHECK(e.setLayerImage(li, tmp + "/aawhite.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            Mapping &m = e.layer(li)->mapping; // a slanted edge
            m.setCorner(0, QPointF(0.1, 0.0)), m.setCorner(1, QPointF(0.6, 0.0)), m.setCorner(2, QPointF(0.9, 1.0)),
                m.setCorner(3, QPointF(0.1, 1.0));
        }
        auto greys = [&] {
            for (int k = 0; k < 2; ++k) e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            int n = 0;
            for (int y = 0; y < g.height(); ++y)
                for (int x = 0; x < g.width(); ++x) {
                    const int v = qGray(g.pixel(x, y));
                    n += v > 25 && v < 230;
                }
            return n;
        };
        Engine::RenderSettings rs;
        rs.samples = 0;
        e.setRenderSettings(rs);
        const int hard = greys();
        rs.samples = 4;
        e.setRenderSettings(rs);
        const int soft = greys();
        std::printf("       edge pixels between black and white: %d without antialiasing, %d with 4x\n", hard, soft);
        CHECK(soft > hard + 20);
        // The project says "default": the machine's default applies
        rs.samples = -1;
        e.setRenderSettings(rs);
        e.setRenderDefaults({0, 4, 0});
        CHECK(e.effectiveRender().samples == 4 && greys() > hard + 20);
        e.setRenderDefaults({0, 0, 0});
        // Mipmaps: a fine checker drawn at an eighth of its size averages to grey instead of aliasing
        CHECK(e.setLayerImage(li, tmp + "/aachecker.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            Mapping &m = e.layer(li)->mapping;
            m.setCorner(0, QPointF(0, 0)), m.setCorner(1, QPointF(0.125, 0)), m.setCorner(2, QPointF(0.125, 0.125)),
                m.setCorner(3, QPointF(0, 0.125));
        }
        auto spread = [&] {
            for (int k = 0; k < 2; ++k) e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            int lo = 255, hi = 0;
            for (int y = 1; y < 7; ++y)
                for (int x = 1; x < 7; ++x) {
                    lo = std::min(lo, qGray(g.pixel(x, y)));
                    hi = std::max(hi, qGray(g.pixel(x, y)));
                }
            return std::pair<int, int>{lo, hi};
        };
        rs = Engine::RenderSettings{-1, 0, 1};
        e.setRenderSettings(rs);
        const auto [lo, hi] = spread();
        CHECK(lo > 90 && hi < 165); // grey everywhere
        // The frame rate is the project's (composition/fps); samples, mipmaps and depth are the machine's
        rs = Engine::RenderSettings{30, 8, 1};
        e.setRenderSettings(rs);
        CHECK(e.saveProject(tmp + "/render.fulskrin", {}, &err));
        e.newProject();
        CHECK(e.renderSettings().frameRate < 0 && e.renderSettings().samples < 0); // a new project: the defaults
        CHECK(e.loadProject(tmp + "/render.fulskrin", nullptr, &err));
        CHECK(e.renderSettings().frameRate == 30 && e.renderSettings().samples < 0 && e.renderSettings().mipmaps < 0);
        e.setRenderSettings({});
    }

    // 5. Render thread
    auto waitFrames = [&](quint64 n) {
        const quint64 target = e.frameCount() + n;
        QElapsedTimer t;
        t.start();
        while (e.frameCount() < target && t.elapsed() < 10000) QThread::msleep(5);
        return e.frameCount() >= target;
    };
    e.newProject();
    CHECK(e.start());
    CHECK(e.isThreaded());
    CHECK(waitFrames(5));
    {
        // The main thread "blocks": output keeps being produced.
        const quint64 before = e.frameCount();
        QThread::msleep(500);
        std::printf("       %llu frames during 500 ms of main-thread blocking\n",
                    static_cast<unsigned long long>(e.frameCount() - before));
        CHECK(e.frameCount() - before >= 5); // still rendering (the rate itself depends on the machine: 2 CPUs give ~12)
    }
    // The frame rate chosen paces the render thread
    {
        e.setRenderSettings({4, -1, -1});
        QThread::msleep(300);
        const quint64 before = e.frameCount();
        QThread::msleep(1000);
        const quint64 n = e.frameCount() - before;
        std::printf("       %llu frames in 1 s at 4 fps\n", static_cast<unsigned long long>(n));
        CHECK(n >= 2 && n <= 6);
        e.setRenderSettings({});
    }
    // The image changes over time (animated generator) in threaded mode
    {
        int pl = e.addLayer("anim");
        e.setLayerIsf(pl, root + "/../isf/generators/Plasma.fs", &err);
        CHECK(waitFrames(5));
        const QImage a = e.grabOutput();
        QThread::msleep(400);
        CHECK(waitFrames(5));
        const QImage b = e.grabOutput();
        std::printf("       identical frames 400 ms apart: %s\n", a == b ? "yes" : "no");
        CHECK(a != b);
        e.removeLayer(pl);
    }
    // Concurrent changes during rendering (small composition: the software GPU used for tests is slow)
    {
        e.setCompositionSize(QSize(320, 180));
        QElapsedTimer t;
        t.start();
        int ops = 0;
        while (t.elapsed() < 3000) {
            int a = e.addLayer("stress");
            e.setLayerIsf(a, root + "/../isf/generators/Plasma.fs", &err);
            e.addEffect(a, root + "/../isf/effects/Blur.fs", &err);
            {
                Engine::Lock lk(&e.mutex());
                Layer *l = e.layer(a);
                l->mapping.setCorner(1, QPointF(0.7, 0.1));
                l->mapping.resetMesh(6, 5);
                l->effects[0]->inputs()[1].fValue = 12;
                l->opacity = 0.8f;
            }
            int b = e.insertLayerJson(V + 0, e.layerJson(a + 1 > e.layerCount() - 1 ? a : a));
            e.setEffectsJson(b, QJsonArray());
            if (e.layerCount() > 6) {
                e.removeLayer(e.layerCount() - 1);
                e.removeLayer(V + 0);
            }
            e.fadeCompositionOpacity(ops % 2 ? 1.0 : 0.5, 0.1);
            ++ops;
        }
        std::printf("       %d rounds of concurrent changes\n", ops);
        CHECK(ops >= 4);
        CHECK(waitFrames(5));
    }
    // Video in threaded mode
    {
        e.newProject();
        e.setCompositionSize(QSize(640, 360));
        int v2 = e.addLayer("v");
        CHECK(e.setLayerVideo(v2, root + "/media/h264.mp4", &err));
        CHECK(waitFrames(20));
        Engine::Lock lk(&e.mutex());
        CHECK(e.layer(v2)->videoTex && e.layer(v2)->videoTex->width() == 1280);
    }
    // Master: full blackout
    {
        e.newProject();
        int c = e.addLayer("white");
        e.setLayerIsf(c, root + "/../isf/generators/SolidColor.fs", &err);
        e.fadeCompositionOpacity(1.0, 0);
        CHECK(waitFrames(4));
        QImage lit = e.grabOutput();
        CHECK(!lit.isNull() && qGray(lit.pixel(lit.width() / 2, lit.height() / 2)) > 240);
        e.fadeCompositionOpacity(0.0, 0);
        CHECK(waitFrames(4));
        QImage dark = e.grabOutput();
        CHECK(!dark.isNull() && qGray(dark.pixel(dark.width() / 2, dark.height() / 2)) < 2);
        e.fadeCompositionOpacity(1.0, 1.0); // fade back up in 1 s
        QThread::msleep(500);
        const double mid = e.compositionOpacity();
        std::printf("       composition opacity at mid-fade: %.2f\n", mid);
        CHECK(mid > 0.25 && mid < 0.75);
    }
    // Color switches: a parameter that is off keeps its value but is not applied
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        const int w = e.addLayer("white");
        e.setLayerIsf(w, root + "/../isf/generators/SolidColor.fs", &err);
        CHECK(waitFrames(4));
        auto center = [&] {
            const QImage im = e.grabOutput();
            return im.isNull() ? QColor() : QColor(im.pixel(im.width() / 2, im.height() / 2));
        };
        CHECK(center().green() > 240);
        {
            Engine::Lock lk(&e.mutex());
            Layer *l = e.layer(w);
            l->color.remove[1] = 1.0f; // green removed
            l->color.removeOn = false;
        }
        CHECK(waitFrames(4));
        CHECK(center().green() > 240); // switched off: the picture is left alone
        {
            Engine::Lock lk(&e.mutex());
            e.layer(w)->color.removeOn = true;
        }
        CHECK(waitFrames(4));
        CHECK(center().green() < 40 && center().red() > 240);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(w)->color.enabled = false; // the whole section
        }
        CHECK(waitFrames(4));
        CHECK(center().green() > 240);
        {
            Engine::Lock lk(&e.mutex());
            CHECK(std::abs(e.layer(w)->color.remove[1] - 1.0f) < 1e-6); // the value is kept
        }
        // Switches saved with the project (a project without them has everything on)
        CHECK(e.saveProject(tmp + "/colorswitch.fulskrin", {}, &err));
        CHECK(e.loadProject(tmp + "/colorswitch.fulskrin", nullptr, &err));
        {
            Engine::Lock lk(&e.mutex());
            Layer *l = e.layer(V + 0);
            CHECK(l && !l->color.enabled && l->color.removeOn && std::abs(l->color.remove[1] - 1.0f) < 1e-6);
        }
        QJsonObject legacy = e.layerJson(V + 0);
        QJsonObject color = legacy.value("color").toObject();
        for (const QString &k : {QStringLiteral("enable"), QStringLiteral("remove_enable")}) color.remove(k);
        legacy["color"] = color;
        e.replaceLayerJson(V + 0, legacy);
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(V + 0)->color.enabled && e.layer(V + 0)->color.removeOn);
        }
    }
    // 6. Publishing: GPU readback -> send thread (BGRA, rows top to bottom)
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setCompositionSize(QSize(64, 32));
        e.setViewportSize(e.viewports().first(), QSize(64, 32)); // what is published is the viewport's picture
        QImage img(64, 32, QImage::Format_RGBA8888);
        img.fill(QColor(255, 0, 0));
        for (int y = 16; y < 32; ++y)
            for (int x = 0; x < 64; ++x) img.setPixelColor(x, y, QColor(0, 0, 255));
        img.save(tmp + "/topred.png");
        int li = e.addLayer("img");
        CHECK(e.setLayerImage(li, tmp + "/topred.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
        }
        // Shared with the send thread, which may still deliver a frame after the tap is removed
        struct TapState {
            std::mutex m;
            CpuFrame last;
            int frames = 0;
        };
        auto tap = std::make_shared<TapState>();
        e.setTestTap([tap](const CpuFrame &f) {
            std::lock_guard<std::mutex> lk(tap->m);
            tap->last = f;
            ++tap->frames;
        });
        CHECK(waitFrames(20));
        std::lock_guard<std::mutex> lk(tap->m);
        const CpuFrame &last = tap->last;
        const int frames = tap->frames;
        std::printf("       %d frames received by the send thread\n", frames);
        CHECK(frames >= 10);
        CHECK(last.width == 64 && last.height == 32 && last.stride == 256);
        auto px = [&](int x, int y) { const uint8_t *p = last.bgra.data() + y * last.stride + x * 4; return QColor(p[2], p[1], p[0]); };
        std::printf("       top pixel %s, bottom %s\n", qPrintable(px(10, 2).name()), qPrintable(px(10, 29).name()));
        CHECK(px(10, 2) == QColor(255, 0, 0));  // top: red
        CHECK(px(10, 29) == QColor(0, 0, 255)); // bottom: blue
        CHECK(last.timestamp100ns > 0);
    }
    e.setTestTap(nullptr);
    {
        // Settings: disabled by default, Syphon/Spout reported according to platform, JSON round trip
        PublishSettings ps;
        ps[PublishKind::Ndi].enabled = true;
        ps[PublishKind::Ndi].name = "Stage — Main";
        ps[PublishKind::Syphon].enabled = true;
        ps.omtQuality = 50;
        CHECK(PublishSettings::fromJson(ps.toJson()) == ps);
        const quint64 vp = e.mainViewportId(); // each viewport publishes its own picture
        e.setPublishSettings(vp, ps);
        CHECK(waitFrames(3));
        const PublishState syphon = e.publishState(vp, PublishKind::Syphon);
        std::printf("       Syphon: %s\n       NDI: %s\n", qPrintable(syphon.text), qPrintable(e.publishState(vp, PublishKind::Ndi).text));
        CHECK(publishCompiledIn(PublishKind::Syphon) ? syphon.level != PublishState::Unavailable
                                                     : syphon.level == PublishState::Unavailable);
        CHECK(e.publishState(vp, PublishKind::Omt).level == PublishState::Off);
        e.setPublishSettings(vp, PublishSettings());
        CHECK(waitFrames(3));
        CHECK(e.publishState(vp, PublishKind::Ndi).level == PublishState::Off);
    }

    // 7. Media: usage, missing file kept, relink, media bin
    {
        e.newProject();
        QFile::copy(root + "/media/h264.mp4", tmp + "/clip.mp4");
        int v = e.addLayer("Clip");
        CHECK(e.setLayerVideo(v, tmp + "/clip.mp4", &err));
        int g = e.addLayer("Mask");
        e.setLayerIsf(g, root + "/../isf/generators/TestPattern.fs", &err);
        int fx = e.addEffect(g, root + "/../isf/effects/Mask.fs", &err);
        CHECK(fx == 0);
        {
            IsfInstance *inst;
            {
                Engine::Lock lk(&e.mutex());
                inst = e.layer(g)->effects[0].get();
            }
            CHECK(e.setIsfImageInput(inst, 1, tmp + "/topred.png", &err));
        }
        e.addBinItems({root + "/media/bars.png", tmp + "/clip.mp4"});
        auto usage = e.mediaUsage();
        CHECK(usage.size() == 3);
        auto find = [&](const QString &p) -> const Engine::MediaRef * {
            for (const auto &r : usage) if (r.path == p) return &r;
            return nullptr;
        };
        const Engine::MediaRef *clip = find(tmp + "/clip.mp4");
        CHECK(clip && clip->video && clip->users == QStringList{"Clip"} && clip->imported && !clip->missing);
        const Engine::MediaRef *mask = find(tmp + "/topred.png");
        CHECK(mask && !mask->video && mask->users.size() == 1 && mask->users[0].contains("Mask"));
        CHECK(e.layersUsingMedia(tmp + "/topred.png") == QList<int>{g});

        // Save, delete the video, reopen: the path is kept and reported
        CHECK(e.saveProject(tmp + "/media.fulskrin", {}, &err));
        QFile::rename(tmp + "/clip.mp4", tmp + "/clip_moved.mp4");
        QString warn;
        CHECK(e.loadProject(tmp + "/media.fulskrin", nullptr, &warn));
        std::printf("       warning: %s\n", qPrintable(warn));
        CHECK(warn.contains("not found"));
        int vi = -1;
        for (int i = 0; i < e.layerCount(); ++i) if (e.layer(i)->name == "Clip") vi = i;
        CHECK(vi >= 0);
        usage = e.mediaUsage();
        clip = find(tmp + "/clip.mp4");
        CHECK(clip && clip->missing && clip->video);
        CHECK(e.layerJson(vi).value("source").toObject().value("type").toString() == "video");
        CHECK(e.binItems().size() == 2);
        // Relink: the video comes back, the mapping is kept
        Mapping before = e.layer(vi)->mapping;
        CHECK(e.relinkLayerMedia(vi, tmp + "/clip.mp4", tmp + "/clip_moved.mp4", &err));
        e.relinkBinItem(tmp + "/clip.mp4", tmp + "/clip_moved.mp4");
        CHECK(e.layer(vi)->type == SourceType::Video && e.layer(vi)->error.isEmpty());
        CHECK(e.layer(vi)->mapping.toJson() == before.toJson());
        CHECK(e.binItems().contains(tmp + "/clip_moved.mp4"));
        QFile::remove(tmp + "/clip_moved.mp4");
        VideoDecoder::Info info;
        CHECK(VideoDecoder::probe(root + "/media/prores.mov", &info) && info.width == 1920 && info.codec == "prores");
    }

    // --- Sound: audio layer, sound of a video layer, synchronized with the playhead
    {
        CHECK(e.startAudio(QString(), &err, true)); // null device: no sound card, but the callback runs in real time
        std::mutex capMutex;
        std::vector<float> cap; // last second of mixed output (stereo)
        std::atomic<long long> tapFrames{0};
        QElapsedTimer tapClock;
        tapClock.start();
        e.audioOutput().setTap([&](const float *s, int n) {
            tapFrames += n;
            std::lock_guard<std::mutex> lk(capMutex);
            cap.insert(cap.end(), s, s + size_t(n) * 2);
            const size_t keep = 48000 * 2;
            if (cap.size() > keep) cap.erase(cap.begin(), cap.begin() + long(cap.size() - keep));
        });
        auto last = [&](double seconds) { // left channel of the last `seconds`
            std::lock_guard<std::mutex> lk(capMutex);
            const size_t n = std::min(cap.size() / 2, size_t(seconds * 48000));
            std::vector<float> l(n);
            for (size_t i = 0; i < n; ++i) l[i] = cap[cap.size() - 2 * n + 2 * i];
            return l;
        };
        auto rms = [](const std::vector<float> &v) {
            double a = 0;
            for (float x : v) a += double(x) * x;
            return v.empty() ? 0.0 : std::sqrt(a / double(v.size()));
        };
        auto freq = [](const std::vector<float> &v) { // zero crossings
            int z = 0;
            for (size_t i = 1; i < v.size(); ++i) z += (v[i - 1] < 0) != (v[i] < 0);
            return v.empty() ? 0.0 : z / 2.0 / (double(v.size()) / 48000);
        };
        auto settle = [](int ms) { QThread::msleep(ms); };
        // Waits (up to 2 s) for a condition on the output: robust on slow machines where frames are late
        auto waitFor = [&](const std::function<bool()> &ok) {
            QElapsedTimer t;
            t.start();
            while (!ok() && t.elapsed() < 2000) QThread::msleep(50);
            return ok();
        };
        auto silent = [&] { return waitFor([&] { return rms(last(0.1)) < 1e-4; }); };
        auto tone = [&](double hz, double tol) {
            return waitFor([&] { return std::abs(freq(last(0.15)) - hz) < tol && rms(last(0.15)) > 0.05; });
        };

        const int a = e.addLayer("Tone");
        CHECK(e.setLayerAudio(a, root + "/media/tone.wav", &err));
        CHECK(e.layer(a)->type == SourceType::Audio && e.layer(a)->audio && std::abs(e.layer(a)->duration() - 4.0) < 0.05);
        CHECK(tone(440, 15));
        settle(400); // let the stream settle before measuring the synchronization
        double worst = 0;
        for (int k = 0; k < 20; ++k) {
            worst = std::max(worst, std::abs(e.layer(a)->audio->syncError()));
            QThread::msleep(50);
        }
        // The test sound card is clocked in software: on an overloaded machine (CI) it runs late, as a real card
        // would during dropouts. A real card is clocked by its quartz (within a few ppm).
        const double cardSpeed = double(tapFrames) / 48000.0 / (tapClock.nsecsElapsed() / 1e9); // since the start
        const bool accurateCard = std::abs(cardSpeed - 1.0) < 0.005;
        std::printf("      worst sync error over 1 s: %.1f ms (test sound card speed %.4f)\n", worst * 1000, cardSpeed);
        // Accurate clock: within 12 ms. Late clock: below the realignment threshold (40 ms), far under the
        // lip-sync detectability threshold (about 45 ms ahead / 125 ms behind, ITU-R BT.1359).
        CHECK(worst < (accurateCard ? 0.012 : 0.045));

        e.seekLayer(a, 3.0); // the sound follows the playhead
        CHECK(tone(880, 20));
        e.seekLayer(a, 0.5);
        CHECK(tone(440, 15));

        e.setLayerPlaying(a, false); // pause: silence, the position stays put
        CHECK(silent());
        double paused;
        {
            Engine::Lock lk(&e.mutex());
            paused = e.layer(a)->position();
        }
        settle(200);
        {
            Engine::Lock lk(&e.mutex());
            CHECK(std::abs(e.layer(a)->position() - paused) < 1e-9);
        }
        e.setLayerPlaying(a, true);
        waitFor([&] { return rms(last(0.2)) > 0.2; });
        settle(100);
        const double full = rms(last(0.2));
        CHECK(full > 0.2);

        e.setLayerVolume(a, 0.5f);
        waitFor([&] { return std::abs(rms(last(0.2)) / full - 0.5) < 0.05; });
        const double half = rms(last(0.2));
        std::printf("      volume 0.5: rms ratio %.3f\n", half / full);
        CHECK(std::abs(half / full - 0.5) < 0.05);
        e.setLayerVolume(a, 1.0f);
        e.setLayerMuted(a, true);
        CHECK(silent());
        e.setLayerMuted(a, false);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->enabled = false; // layer off = no sound either
        }
        CHECK(silent());
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->enabled = true;
        }
        e.setAudioVolume(0.0f); // master
        CHECK(silent());
        e.setAudioVolume(1.0f);
        CHECK(waitFor([&] { return rms(last(0.2)) > 0.2; }));

        // Loop: across the end of the file, no gap and no resync
        e.seekLayer(a, 3.6);
        settle(250);
        const int resyncs = e.layer(a)->audio->resyncCount();
        settle(500); // wraps around at 4 s
        std::vector<float> wrapped = last(0.5);
        double minRms = 1;
        for (size_t k = 0; k + 2400 <= wrapped.size(); k += 2400)
            minRms = std::min(minRms, rms(std::vector<float>(wrapped.begin() + long(k), wrapped.begin() + long(k + 2400))));
        CHECK(minRms > 0.2);
        CHECK(e.layer(a)->audio->resyncCount() == resyncs);
        CHECK(std::abs(freq(last(0.1)) - 440) < 20);

        // Speed: tape-style, the pitch follows
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->speed = 1.5;
        }
        e.seekLayer(a, 0.2);
        CHECK(tone(660, 25));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->speed = 1.0;
        }

        // Save / load
        const QJsonObject j = e.layerJson(a);
        CHECK(j.value("source").toObject().value("type").toString() == "audio");
        CHECK(j.contains("volume") && j.contains("muted"));

        // Video with a sound track
        e.removeLayer(a);
        CHECK(silent());
        const int v = e.addLayer("AV");
        CHECK(e.setLayerVideo(v, root + "/media/av.mp4", &err));
        CHECK(e.layer(v)->video && e.layer(v)->audio);
        CHECK(tone(660, 20));
        settle(500);
        CHECK(std::abs(e.layer(v)->audio->syncError()) < 0.045);
        const int nv = e.addLayer("Silent video");
        CHECK(e.setLayerVideo(nv, root + "/media/h264.mp4", &err));
        CHECK(!e.layer(nv)->audio); // no audio track: no stream
        e.removeLayer(nv);

        // Media bin
        e.addBinItems({root + "/media/tone.wav"});
        bool sawAudio = false;
        for (const auto &r : e.mediaUsage()) sawAudio |= r.path.endsWith("tone.wav") && r.audio && !r.video;
        CHECK(sawAudio);
        e.removeBinItem(root + "/media/tone.wav");
        AudioStream::Info ai;
        CHECK(AudioStream::probe(root + "/media/tone.wav", &ai) && ai.sampleRate == 44100 && ai.channels == 1);
        CHECK(!AudioStream::probe(root + "/media/h264.mp4", &ai));

        // --- Play modes (the video with sound of the previous test is muted meanwhile)
        e.setLayerMuted(v, true);
        { // Video ping-pong: the backward leg delivers the frames in reverse order (across GOPs)
            VideoDecoder d;
            CHECK(d.open(root + "/media/index.mp4", &err));
            Timeline tl;
            tl.mode = Timeline::PingPong;
            d.setTimeline(tl);
            d.seek(0);
            std::vector<uint8_t> buf;
            int w = 0, h = 0, wrong = 0, missing = 0;
            const double dt = 1.0 / 25, span = 2.0;
            for (int k = 0; k < 150; ++k) { // forward, backward, forward again
                const double t = k * dt + dt / 2;
                QElapsedTimer tm;
                tm.start();
                bool got = false;
                while (!(got = d.fetchRgba(t, buf, &w, &h)) && tm.elapsed() < 3000) QThread::msleep(1);
                if (!got) {
                    ++missing;
                    continue;
                }
                const double x = std::fmod(t, 2 * span);
                const int expected = x < span ? int(x / dt) : std::min(49, int((2 * span - x) / dt));
                const int r = buf[size_t((h / 2) * w + w / 2) * 4];
                const int frame = int(std::lround((r * 219.0 / 255.0 + 16.0 - 20.0) / 4.0));
                if (std::abs(frame - expected) > 1) {
                    if (wrong < 5) std::printf("      t=%.2f frame %d expected %d\n", t, frame, expected);
                    ++wrong;
                }
            }
            CHECK(missing == 0);
            CHECK(wrong == 0);
        }
        { // Sound in ping-pong: backwards after the end (880 Hz part), then the start (440 Hz), no resync at the turns
            const int p = e.addLayer("Ping-pong");
            CHECK(e.setLayerAudio(p, root + "/media/tone.wav", &err));
            e.setLayerPlayMode(p, PlayMode::PingPong);
            e.seekLayer(p, 3.5);
            CHECK(tone(880, 20));
            settle(200);
            const int resyncs = e.layer(p)->audio->resyncCount();
            QElapsedTimer t;
            t.start();
            double minRms = 1;
            while (t.elapsed() < 1000) { // the turn at 4 s
                minRms = std::min(minRms, rms(last(0.05)));
                QThread::msleep(40);
            }
            double phase;
            {
                Engine::Lock lk(&e.mutex());
                phase = e.layer(p)->clock;
                CHECK(e.layer(p)->position() > 2.5 && e.layer(p)->position() < 3.9); // on the way back
            }
            std::printf("      ping-pong clock %.2f, min rms %.3f\n", phase, minRms);
            CHECK(minRms > 0.2);
            CHECK(std::abs(freq(last(0.15)) - 880) < 20);
            CHECK(tone(440, 15)); // backwards below 2 s
            CHECK(e.layer(p)->audio->resyncCount() == resyncs);
            e.removeLayer(p);
        }
        { // Negative speed, video: backwards from 1 s, then the loop goes on backwards from the end
            VideoDecoder d;
            CHECK(d.open(root + "/media/index.mp4", &err));
            Timeline tl;
            tl.mode = Timeline::Loop;
            tl.origin = 1.0;
            tl.dir = -1;
            d.setTimeline(tl);
            d.seek(0);
            tl.duration = 2.0;
            std::vector<uint8_t> buf;
            int w = 0, h = 0, wrong = 0, missing = 0;
            const double dt = 1.0 / 25;
            for (int k = 0; k < 120; ++k) {
                const double c = k * dt + dt / 2;
                QElapsedTimer tm;
                tm.start();
                bool got = false;
                while (!(got = d.fetchRgba(c, buf, &w, &h)) && tm.elapsed() < 3000) QThread::msleep(1);
                if (!got) {
                    ++missing;
                    continue;
                }
                const int expected = std::min(49, int(tl.position(c) / dt));
                const int r = buf[size_t((h / 2) * w + w / 2) * 4];
                const int frame = int(std::lround((r * 219.0 / 255.0 + 16.0 - 20.0) / 4.0));
                if (std::abs(frame - expected) > 1) {
                    if (wrong < 5) std::printf("      reverse c=%.2f frame %d expected %d\n", c, frame, expected);
                    ++wrong;
                }
            }
            CHECK(missing == 0);
            CHECK(wrong == 0);
        }
        { // Negative speed, sound: backwards (880 Hz part, then 440 Hz), across 0 in Loop, then forwards again
            const int r = e.addLayer("Reverse");
            CHECK(e.setLayerAudio(r, root + "/media/tone.wav", &err));
            e.setLayerSpeed(r, -1.0);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(r)->dir == -1 && e.layer(r)->position() > 3.9); // starts from the end
            }
            e.seekLayer(r, 2.8);
            CHECK(tone(880, 20));
            CHECK(tone(440, 15)); // below 2 s, backwards
            double p1, p2;
            {
                Engine::Lock lk(&e.mutex());
                p1 = e.layer(r)->position();
            }
            settle(300);
            {
                Engine::Lock lk(&e.mutex());
                p2 = e.layer(r)->position();
            }
            std::printf("      backwards: %.2f -> %.2f\n", p1, p2);
            CHECK(p2 < p1 - 0.2);
            e.seekLayer(r, 0.3); // Loop: after 0, backwards from the end (880 Hz)
            CHECK(tone(440, 15));
            settle(100);
            const int resyncs = e.layer(r)->audio->resyncCount();
            CHECK(tone(880, 20));
            CHECK(e.layer(r)->audio->resyncCount() == resyncs); // the wrap is seamless
            // Direction change keeps the position
            e.seekLayer(r, 2.5);
            settle(150);
            double before, after;
            {
                Engine::Lock lk(&e.mutex());
                before = e.layer(r)->position();
            }
            e.setLayerSpeed(r, 1.0);
            {
                Engine::Lock lk(&e.mutex());
                after = e.layer(r)->position();
                CHECK(e.layer(r)->dir == 1);
            }
            CHECK(std::abs(after - before) < 0.05);
            settle(400);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(r)->position() > after + 0.25);
            }
            CHECK(waitFor([&] { return rms(last(0.1)) > 0.2; }));
            // One-shot backwards: stops at the start
            e.setLayerPlayMode(r, PlayMode::OneShot);
            e.setLayerSpeed(r, -2.0);
            e.seekLayer(r, 0.4);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(r)->playing;
            }));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(r)->position() < 1e-6);
            }
            CHECK(e.layerJson(r).value("source").toObject().value("speed").toDouble() == -2.0);
            e.removeLayer(r);
        }
        { // In / out points, video: the loop stays within [0.4, 1.2] s, frame by frame
            VideoDecoder d;
            CHECK(d.open(root + "/media/index.mp4", &err));
            Timeline tl;
            tl.mode = Timeline::Loop;
            tl.in = 0.4;
            tl.out = 1.2;
            tl.origin = 0.4;
            d.setTimeline(tl);
            d.seek(0);
            tl.duration = 2.0;
            std::vector<uint8_t> buf;
            int w = 0, h = 0, wrong = 0, missing = 0, outside = 0;
            const double dt = 1.0 / 25;
            for (int k = 0; k < 60; ++k) { // 3 loops of 0.8 s
                const double c = k * dt + dt / 2;
                QElapsedTimer tm;
                tm.start();
                bool got = false;
                while (!(got = d.fetchRgba(c, buf, &w, &h)) && tm.elapsed() < 3000) QThread::msleep(1);
                if (!got) {
                    ++missing;
                    continue;
                }
                const int expected = int(tl.position(c) / dt);
                const int r = buf[size_t((h / 2) * w + w / 2) * 4];
                const int frame = int(std::lround((r * 219.0 / 255.0 + 16.0 - 20.0) / 4.0));
                if (frame < 10 - 1 || frame > 29 + 1) ++outside;
                if (std::abs(frame - expected) > 1) {
                    if (wrong < 5) std::printf("      in/out c=%.2f frame %d expected %d\n", c, frame, expected);
                    ++wrong;
                }
            }
            CHECK(missing == 0);
            CHECK(wrong == 0 && outside == 0);
        }
        { // In / out points, sound: loop over [1.5, 2.5] s straddling the 440 / 880 Hz change, seamless
            const int a2 = e.addLayer("Range");
            CHECK(e.setLayerAudio(a2, root + "/media/tone.wav", &err));
            e.setLayerInOut(a2, 1.5, 2.5);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(a2)->position() >= 1.5 - 1e-9 && e.layer(a2)->position() <= 2.5 + 1e-9);
            }
            e.seekLayer(a2, 1.6);
            CHECK(tone(440, 15));
            settle(150);
            const int resyncs = e.layer(a2)->audio->resyncCount();
            bool saw880 = false, saw440After = false, outside = false;
            double minRms = 1;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 1800) { // more than one loop
                double pos;
                {
                    Engine::Lock lk(&e.mutex());
                    pos = e.layer(a2)->position();
                }
                outside |= pos < 1.5 - 1e-6 || pos > 2.5 + 1e-6;
                const double f = freq(last(0.08));
                if (std::abs(f - 880) < 30) saw880 = true;
                if (saw880 && std::abs(f - 440) < 20) saw440After = true; // back to the in point
                minRms = std::min(minRms, rms(last(0.05)));
                QThread::msleep(40);
            }
            CHECK(!outside);
            CHECK(saw880 && saw440After);
            CHECK(minRms > 0.2);
            CHECK(e.layer(a2)->audio->resyncCount() == resyncs);
            // One-shot stops at the out point; backwards, at the in point
            e.setLayerPlayMode(a2, PlayMode::OneShot);
            e.seekLayer(a2, 2.3);
            e.setLayerPlaying(a2, true);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(a2)->playing;
            }));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(std::abs(e.layer(a2)->position() - 2.5) < 1e-6);
            }
            e.setLayerSpeed(a2, -1.0);
            e.setLayerPlaying(a2, true); // at its end: starts again from the out point, backwards
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(a2)->playing;
            }));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(std::abs(e.layer(a2)->position() - 1.5) < 1e-6);
            }
            const QJsonObject j = e.layerJson(a2).value("source").toObject();
            CHECK(std::abs(j.value("in").toDouble() - 1.5) < 1e-9 && std::abs(j.value("out").toDouble() - 2.5) < 1e-9);
            CHECK(e.setLayerAudio(a2, root + "/media/tone.wav", &err)); // a new media is played whole
            CHECK(e.layer(a2)->inPoint == 0 && e.layer(a2)->outPoint < 0);
            e.removeLayer(a2);
        }
        { // Stop: black and silent at the end; One-shot: last frame kept
            const int s1 = e.addLayer("Stop");
            CHECK(e.setLayerVideo(s1, root + "/media/av.mp4", &err));
            e.setLayerPlayMode(s1, PlayMode::Stop);
            e.seekLayer(s1, 3.7);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return e.layer(s1)->ended;
            }));
            settle(100);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(!e.layer(s1)->playing && e.layer(s1)->finalTex == 0);
            }
            CHECK(silent());
            e.setLayerPlayMode(s1, PlayMode::OneShot);
            e.seekLayer(s1, 3.7);
            e.setLayerPlaying(s1, true);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(s1)->playing;
            }));
            settle(100);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(!e.layer(s1)->ended && e.layer(s1)->finalTex != 0);
            }
            CHECK(e.layerJson(s1).value("source").toObject().value("play_mode").toString() == "oneshot");
            // Default mode (preference) given to a newly loaded video
            e.setDefaultPlayMode(PlayMode::PingPong);
            CHECK(e.setLayerVideo(s1, root + "/media/h264.mp4", &err));
            CHECK(e.layer(s1)->mode == PlayMode::PingPong);
            e.setDefaultPlayMode(PlayMode::Loop);
            e.removeLayer(s1);
        }
        e.setLayerMuted(v, false);

        e.removeLayer(e.layerCount() > v ? v : 0);
        e.audioOutput().setTap(nullptr);
        e.stopAudio();
    }

    e.stop();
    CHECK(!e.isThreaded());
    e.renderFrame(); // back to manual mode
    CHECK(!e.grabOutput().isNull());

    // 12. Copy / paste of layer parameters, and a layer used as the source of another (pre-FX / post-FX)
    {
        e.newProject();
        const QString isfDir = root + "/../isf";
        const quint64 idA = e.layerId(e.addLayer("A"));
        CHECK(e.setLayerIsf(e.indexOfId(idA), isfDir + "/generators/TestPattern.fs", &err));
        // B is above A in the list: the render pass must still render A first (B's source)
        const quint64 idB = e.layerId(e.addLayer("B", V + 0));
        CHECK(e.setLayerIsf(e.indexOfId(idB), isfDir + "/generators/SolidColor.fs", &err));
        auto A = [&] { return e.indexOfId(idA); };
        auto B = [&] { return e.indexOfId(idB); };
        CHECK(A() == V + 1 && B() == V + 0);

        // A: a distinctive value in every part
        {
            Engine::Lock lk(&e.mutex());
            Layer *l = e.layer(A());
            l->roi = QRectF(QPointF(0.25, 0.1), QPointF(0.75, 0.6));
            l->color.temp = 1000;
            l->color.tintOn = false;
            l->opacity = 0.25f;
            l->blend = BlendMode::Multiply;
            l->mapping.setCorner(1, QPointF(0.8, 0.2));
        }
        CHECK(e.addEffect(A(), isfDir + "/effects/Hue.fs", &err) == 0);
        const QJsonObject clip = e.layerJson(A());

        // One part at a time: nothing else moves
        CHECK(e.applyLayerParts(B(), clip, Engine::PartRoi));
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(B());
            CHECK(l->roi == QRectF(QPointF(0.25, 0.1), QPointF(0.75, 0.6)));
            CHECK(l->color.temp == 0.f && l->opacity == 1.f && l->effects.empty());
            CHECK(l->mapping.corners[1] == QPointF(1, 0));
            CHECK(l->type == SourceType::Isf && l->sourcePath.endsWith("SolidColor.fs")); // its own media kept
        }
        CHECK(e.applyLayerParts(B(), clip, Engine::PartColor));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->color.temp == 1000.f && !e.layer(B())->color.tintOn);
            CHECK(e.layer(B())->opacity == 1.f);
        }
        CHECK(e.applyLayerParts(B(), clip, Engine::PartSpatial | Engine::PartCompositing));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->mapping.corners[1] == QPointF(0.8, 0.2));
            CHECK(e.layer(B())->opacity == 0.25f && e.layer(B())->blend == BlendMode::Multiply);
            CHECK(e.layer(B())->effects.empty());
        }
        CHECK(e.applyLayerParts(B(), clip, Engine::PartEffects));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->effects.size() == 1 && e.layer(B())->effects[0]->name() == "Hue");
            CHECK(e.layer(B())->type == SourceType::Isf && e.layer(B())->sourcePath.endsWith("SolidColor.fs"));
        }
        // Everything, the source included: B becomes a copy of A, with its own name and place
        CHECK(e.applyLayerParts(B(), clip, Engine::PartAll));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->name == "B" && e.layer(B())->sourcePath.endsWith("TestPattern.fs"));
        }
        CHECK(B() == V + 0 && e.layerId(V + 0) == idB);
        // A locked layer refuses a paste
        {
            Engine::Lock lk(&e.mutex());
            e.layer(B())->locked = true;
        }
        CHECK(!e.applyLayerParts(B(), clip, Engine::PartRoi));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(B())->locked = false;
            e.layer(B())->roi = Layer::fullRoi();
        }

        // B's picture is A's, after A's effect chain
        CHECK(e.setLayerSourceLayer(B(), idA, LayerTap::PostFx, &err));
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->type == SourceType::Layer && e.layer(B())->rawTex != 0);
            CHECK(e.layer(B())->rawTex == e.layer(A())->finalTex);
            CHECK(e.layer(A())->preFxTex != e.layer(A())->finalTex); // A has a ROI, a color and an effect
        }
        // ... or before it
        CHECK(e.setLayerTap(B(), LayerTap::PreFx));
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->rawTex == e.layer(A())->preFxTex);
        }
        // Refused: itself, and anything that would feed the picture back on itself
        CHECK(!e.setLayerSourceLayer(B(), idB, LayerTap::PostFx, &err) && !err.isEmpty());
        CHECK(!e.setLayerSourceLayer(A(), idB, LayerTap::PostFx, &err) && !err.isEmpty());
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layerDependsOn(idB, idA) && !e.layerDependsOn(idA, idB));
        }
        // A group has no source of its own
        const int g = e.addGroup("G", V + 0);
        CHECK(!e.setLayerSourceLayer(g, idA, LayerTap::PostFx, &err));
        // A hidden group used as a source is rendered all the same
        const quint64 idG = e.layerId(g);
        e.setStructure(tree::intoGroup(e.structure(), {idA}, idG));
        CHECK(e.setLayerSourceLayer(B(), idG, LayerTap::PostFx, &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(idG))->enabled = false;
        }
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(e.indexOfId(idG))->finalTex != 0 && e.layer(e.indexOfId(idG))->referenced);
            CHECK(e.layer(B())->rawTex == e.layer(e.indexOfId(idG))->finalTex);
        }
        // A member of a group cannot use its own group: the group is made of it
        CHECK(!e.setLayerSourceLayer(e.indexOfId(idA), idG, LayerTap::PostFx, &err));

        // Saved and read back
        CHECK(e.saveProject(tmp + "/layersrc.fulskrin", {}, &err));
        const QJsonObject saved = readJson(tmp + "/layersrc.fulskrin");
        err.clear(); // the refusals above left their message there
        CHECK(e.loadProject(tmp + "/layersrc.fulskrin", nullptr, &err) && err.isEmpty());
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(e.indexOfId(idB));
            CHECK(l && l->type == SourceType::Layer && l->sourceLayer == idG && l->sourceTap == LayerTap::PostFx);
        }
        CHECK(e.saveProject(tmp + "/layersrc2.fulskrin", {}, &err));
        CHECK(saved.value("layers") == readJson(tmp + "/layersrc2.fulskrin").value("layers"));

        // The layer it used is gone, or a hand-edited project loops: the source is dropped, with a warning
        e.removeLayer(e.indexOfId(idG)); // removes the group and its member
        QStringList warnings;
        e.fixLayerReferences(&warnings);
        CHECK(warnings.size() == 1 && warnings[0].contains("gone"));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(e.indexOfId(idB))->type == SourceType::None);
        }
        e.newProject();
    }

    std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL OK", failures);
    e.shutdown();
    return failures ? 1 : 0;
}
