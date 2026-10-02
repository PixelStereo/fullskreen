// Headless engine tests: load/save, video, ISF (multi-pass, .vs, errors).
#include "Commands.h"
#include "Engine.h"
#include <QElapsedTimer>
#include <QLineF>
#include <mutex>
#include <QUndoStack>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLineF>
#include <QSurfaceFormat>
#include <QThread>
#include <cmath>
#include <atomic>
#include <functional>
#include <cstdio>

static int failures = 0;
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

    // 1. JSON round trip (project built here: generator + warped mesh, video + effects)
    const QString isf = root + "/../isf";
    int gen = e.addLayer("Test Pattern");
    CHECK(e.setLayerIsf(gen, isf + "/generators/TestPattern.fs", &err));
    e.layer(gen)->mapping.meshMode = true;
    e.layer(gen)->mapping.setControlPoint(1, 1, QPointF(0.4, 0.3));
    e.layer(gen)->mapping.setCorner(1, QPointF(0.9, 0.1));
    int vid = e.addLayer("Video", 1);
    CHECK(e.setLayerVideo(vid, root + "/media/h264.mp4", &err));
    e.addEffect(vid, isf + "/effects/ColorCorrection.fs", &err);
    e.addEffect(vid, isf + "/effects/Trails.fs", &err);
    e.layer(vid)->effects[0]->inputs()[1].fValue = 0.5;
    e.layer(vid)->blend = BlendMode::Screen;
    CHECK(e.layerCount() == 2);
    for (int i = 0; i < 10; ++i) e.renderFrame();
    const QPointF cp = e.layer(gen)->mapping.controlPoint(1, 1);
    CHECK(e.saveProject(tmp + "/a.fulskrin", {}, &err));
    CHECK(e.loadProject(tmp + "/a.fulskrin", nullptr, &err));
    CHECK(err.isEmpty());
    CHECK(e.saveProject(tmp + "/b.fulskrin", {}, &err));
    QJsonObject a = readJson(tmp + "/a.fulskrin"), b = readJson(tmp + "/b.fulskrin");
    CHECK(a.value("layers") == b.value("layers"));
    CHECK(e.layer(0)->mapping.meshMode);
    CHECK(QLineF(e.layer(0)->mapping.controlPoint(1, 1), cp).length() < 1e-9);
    CHECK(e.layer(1)->blend == BlendMode::Screen);
    CHECK(e.layer(1)->effects.size() == 2 && std::abs(e.layer(1)->effects[0]->inputs()[1].fValue - 0.5) < 1e-9);

    // 2. Video: seeking and end of playback without loop
    e.newProject();
    int v = e.addLayer("v");
    CHECK(e.setLayerVideo(v, root + "/media/h264.mp4", &err));
    CHECK(std::abs(e.layer(v)->duration() - 4.0) < 0.1);
    CHECK(e.layer(v)->video->width() == 1280);
    e.setLayerLoop(v, false);
    e.seekLayer(v, 3.9);
    { // until the end of the media is reached and a frame uploaded (slow machines: up to 5 s)
        QElapsedTimer t;
        t.start();
        do {
            e.renderFrame();
            QThread::msleep(10);
        } while ((e.layer(v)->playing || e.layer(v)->sourceTex.w != 1280) && t.elapsed() < 5000);
    }
    CHECK(!e.layer(v)->playing);           // stops at end of media
    CHECK(e.layer(v)->sourceTex.w == 1280); // a frame was indeed uploaded to the GPU
    e.setLayerPlaying(v, true);             // restarts from the beginning
    CHECK(e.layer(v)->playhead < 0.01);

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
        CHECK(e.layerCount() == 0);
        undo.undo();
        CHECK(e.layerCount() == 1);
        CHECK(e.layerJson(0) == snap);
        // Effects
        const QJsonArray fxBefore = e.effectsJson(0);
        e.addEffect(0, root + "/../isf/effects/Hue.fs", &err);
        undo.push(new cmd::SetEffects(&e, 0, fxBefore, "effect"));
        CHECK(e.layer(0)->effects.size() == 1);
        undo.undo();
        CHECK(e.layer(0)->effects.empty());
        undo.redo();
        CHECK(e.layer(0)->effects.size() == 1);
        // Back to the very start of the stack
        while (undo.canUndo()) undo.undo();
        CHECK(e.layerCount() == 0);
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
        CHECK(e.frameCount() - before >= 15);
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
            int b = e.insertLayerJson(0, e.layerJson(a + 1 > e.layerCount() - 1 ? a : a));
            e.setEffectsJson(b, QJsonArray());
            if (e.layerCount() > 6) {
                e.removeLayer(e.layerCount() - 1);
                e.removeLayer(0);
            }
            e.fadeMaster(ops % 2 ? 1.0 : 0.5, 0.1);
            ++ops;
        }
        std::printf("       %d rounds of concurrent changes\n", ops);
        CHECK(ops >= 5);
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
        CHECK(e.layer(v2)->sourceTex.w == 1280);
    }
    // Master: full blackout
    {
        e.newProject();
        int c = e.addLayer("white");
        e.setLayerIsf(c, root + "/../isf/generators/SolidColor.fs", &err);
        e.fadeMaster(1.0, 0);
        CHECK(waitFrames(4));
        QImage lit = e.grabOutput();
        CHECK(!lit.isNull() && qGray(lit.pixel(lit.width() / 2, lit.height() / 2)) > 240);
        e.fadeMaster(0.0, 0);
        CHECK(waitFrames(4));
        QImage dark = e.grabOutput();
        CHECK(!dark.isNull() && qGray(dark.pixel(dark.width() / 2, dark.height() / 2)) < 2);
        e.fadeMaster(1.0, 1.0); // fade back up in 1 s
        QThread::msleep(500);
        const double mid = e.masterLevel();
        std::printf("       master level at mid-fade: %.2f\n", mid);
        CHECK(mid > 0.25 && mid < 0.75);
    }
    // 6. Publishing: GPU readback -> send thread (BGRA, rows top to bottom)
    {
        e.newProject();
        e.fadeMaster(1.0, 0);
        e.setCompositionSize(QSize(64, 32));
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
        std::mutex m;
        CpuFrame last;
        int frames = 0;
        e.setTestTap([&](const CpuFrame &f) {
            std::lock_guard<std::mutex> lk(m);
            last = f;
            ++frames;
        });
        CHECK(waitFrames(20));
        std::lock_guard<std::mutex> lk(m);
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
        e.setPublishSettings(ps);
        CHECK(waitFrames(3));
        const PublishState syphon = e.publishState(PublishKind::Syphon);
        std::printf("       Syphon: %s\n       NDI: %s\n", qPrintable(syphon.text), qPrintable(e.publishState(PublishKind::Ndi).text));
        CHECK(publishCompiledIn(PublishKind::Syphon) ? syphon.level != PublishState::Unavailable
                                                     : syphon.level == PublishState::Unavailable);
        CHECK(e.publishState(PublishKind::Omt).level == PublishState::Off);
        e.setPublishSettings(PublishSettings());
        CHECK(waitFrames(3));
        CHECK(e.publishState(PublishKind::Ndi).level == PublishState::Off);
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
            e.layer(a)->visible = false; // layer off = no sound either
        }
        CHECK(silent());
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->visible = true;
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

        e.removeLayer(e.layerCount() > v ? v : 0);
        e.audioOutput().setTap(nullptr);
        e.stopAudio();
    }

    e.stop();
    CHECK(!e.isThreaded());
    e.renderFrame(); // back to manual mode
    CHECK(!e.grabOutput().isNull());

    std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL OK", failures);
    e.shutdown();
    return failures ? 1 : 0;
}
