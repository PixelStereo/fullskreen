// Tests du moteur sans interface : chargement/sauvegarde, vidéo, ISF (multi-passes, .vs, erreurs).
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
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) { std::printf("ÉCHEC %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
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
    const QString tmp = QDir::tempPath() + "/lanterne_test";
    QDir().mkpath(tmp);

    Engine e;
    QString err;
    CHECK(e.initialize(&err));

    // Mode compatibilité : lanterne_tests --check-isf <dossier>  -> compile et rend chaque shader du dossier
    if (argc >= 3 && QString(argv[1]) == "--check-isf") {
        QDirIterator it(argv[2], {"*.fs"}, QDir::Files, QDirIterator::Subdirectories);
        int total = 0, ok = 0, skipped = 0;
        int v = e.addLayer("src");
        e.setLayerVideo(v, root + "/media/h264.mp4", &err);
        int g = e.addLayer("gen");
        while (it.hasNext()) {
            const QString path = it.next();
            IsfInstance::Header h = IsfInstance::readHeader(path);
            if (!h.ok) { std::printf("EN-TÊTE  %s\n", qPrintable(path)); ++total; continue; }
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
            else std::printf("ÉCHEC    %s\n%s\n", qPrintable(QFileInfo(path).fileName()), qPrintable(msg.left(600)));
        }
        std::printf("\nISF : %d/%d compilés (%d transitions ignorées)\n", ok, total, skipped);
        e.shutdown();
        return ok == total ? 0 : 1;
    }

    // 1. Aller-retour JSON (projet construit ici : générateur + grille déformée, vidéo + effets)
    const QString isf = root + "/../isf";
    int gen = e.addLayer("Mire");
    CHECK(e.setLayerIsf(gen, isf + "/generateurs/Mire.fs", &err));
    e.layer(gen)->mapping.meshMode = true;
    e.layer(gen)->mapping.setControlPoint(1, 1, QPointF(0.4, 0.3));
    e.layer(gen)->mapping.setCorner(1, QPointF(0.9, 0.1));
    int vid = e.addLayer("Vidéo", 1);
    CHECK(e.setLayerVideo(vid, root + "/media/h264.mp4", &err));
    e.addEffect(vid, isf + "/effets/Couleur.fs", &err);
    e.addEffect(vid, isf + "/effets/Remanence.fs", &err);
    e.layer(vid)->effects[0]->inputs()[1].fValue = 0.5;
    e.layer(vid)->blend = BlendMode::Screen;
    CHECK(e.layerCount() == 2);
    for (int i = 0; i < 10; ++i) e.renderFrame();
    const QPointF cp = e.layer(gen)->mapping.controlPoint(1, 1);
    CHECK(e.saveProject(tmp + "/a.lanterne", {}, &err));
    CHECK(e.loadProject(tmp + "/a.lanterne", nullptr, &err));
    CHECK(err.isEmpty());
    CHECK(e.saveProject(tmp + "/b.lanterne", {}, &err));
    QJsonObject a = readJson(tmp + "/a.lanterne"), b = readJson(tmp + "/b.lanterne");
    CHECK(a.value("layers") == b.value("layers"));
    CHECK(e.layer(0)->mapping.meshMode);
    CHECK(QLineF(e.layer(0)->mapping.controlPoint(1, 1), cp).length() < 1e-9);
    CHECK(e.layer(1)->blend == BlendMode::Screen);
    CHECK(e.layer(1)->effects.size() == 2 && std::abs(e.layer(1)->effects[0]->inputs()[1].fValue - 0.5) < 1e-9);

    // 2. Vidéo : positionnement et fin de lecture sans boucle
    e.newProject();
    int v = e.addLayer("v");
    CHECK(e.setLayerVideo(v, root + "/media/h264.mp4", &err));
    CHECK(std::abs(e.layer(v)->duration() - 4.0) < 0.1);
    CHECK(e.layer(v)->video->width() == 1280);
    e.setLayerLoop(v, false);
    e.seekLayer(v, 3.9);
    for (int i = 0; i < 30; ++i) { e.renderFrame(); QThread::msleep(10); }
    CHECK(!e.layer(v)->playing);           // arrêt en fin de média
    CHECK(e.layer(v)->sourceTex.w == 1280); // une image a bien été envoyée au GPU
    e.setLayerPlaying(v, true);             // relance depuis le début
    CHECK(e.layer(v)->playhead < 0.01);

    // 3. ISF : vertex shader personnalisé, shader cassé, passes à taille calculée
    int fx = e.addEffect(v, root + "/isf/Decalage.fs", &err);
    CHECK(e.layer(v)->effects[size_t(fx)]->isValid());
    QFile bad(tmp + "/Casse.fs");
    bad.open(QIODevice::WriteOnly);
    bad.write("/*{ \"INPUTS\": [ {\"NAME\":\"inputImage\",\"TYPE\":\"image\"}, ] }*/\nvoid main(){ gl_FragColor = undefinedThing; }\n");
    bad.close();
    int fx2 = e.addEffect(v, tmp + "/Casse.fs", &err);
    CHECK(!e.layer(v)->effects[size_t(fx2)]->isValid());
    CHECK(e.layer(v)->effects[size_t(fx2)]->error().contains("undefinedThing"));
    int fx3 = e.addEffect(v, root + "/../isf/effets/Flou.fs", &err);
    CHECK(e.layer(v)->effects[size_t(fx3)]->isValid());
    for (int i = 0; i < 5; ++i) e.renderFrame(); // le shader cassé laisse passer l'image
    QImage out = e.grabOutput();
    CHECK(!out.isNull());

    // 4. Annuler / rétablir (mode manuel)
    {
        QUndoStack undo;
        e.newProject();
        int g = e.addLayer("Mire");
        e.setLayerIsf(g, root + "/../isf/generateurs/Mire.fs", &err);
        undo.push(new cmd::AddLayer(&e, g, "ajout"));
        // Paramètre : deux mouvements rapprochés fusionnent en une seule étape
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
        undo.push(new cmd::SetMapping(&e, g, before, after, "coin"));
        CHECK(e.layer(g)->mapping.corners[0] == QPointF(0.2, 0.2));
        undo.undo();
        CHECK(e.layer(g)->mapping.corners[0] == before.corners[0]);
        undo.redo();
        // Suppression puis restauration à l'identique
        const QJsonObject snap = e.layerJson(g);
        undo.push(new cmd::RemoveLayer(&e, g));
        CHECK(e.layerCount() == 0);
        undo.undo();
        CHECK(e.layerCount() == 1);
        CHECK(e.layerJson(0) == snap);
        // Effets
        const QJsonArray fxBefore = e.effectsJson(0);
        e.addEffect(0, root + "/../isf/effets/Teinte.fs", &err);
        undo.push(new cmd::SetEffects(&e, 0, fxBefore, "effet"));
        CHECK(e.layer(0)->effects.size() == 1);
        undo.undo();
        CHECK(e.layer(0)->effects.empty());
        undo.redo();
        CHECK(e.layer(0)->effects.size() == 1);
        // Retour au tout début de la pile
        while (undo.canUndo()) undo.undo();
        CHECK(e.layerCount() == 0);
    }

    // 5. Fil de rendu
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
        // Le fil principal « bloque » : la sortie continue d'être produite.
        const quint64 before = e.frameCount();
        QThread::msleep(500);
        std::printf("       %llu images pendant 500 ms de blocage du fil principal\n",
                    static_cast<unsigned long long>(e.frameCount() - before));
        CHECK(e.frameCount() - before >= 15);
    }
    // Modifications concurrentes pendant le rendu (petite composition : le GPU logiciel des tests est lent)
    {
        e.setCompositionSize(QSize(320, 180));
        QElapsedTimer t;
        t.start();
        int ops = 0;
        while (t.elapsed() < 3000) {
            int a = e.addLayer("stress");
            e.setLayerIsf(a, root + "/../isf/generateurs/Plasma.fs", &err);
            e.addEffect(a, root + "/../isf/effets/Flou.fs", &err);
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
        std::printf("       %d séries de modifications concurrentes\n", ops);
        CHECK(ops >= 5);
        CHECK(waitFrames(5));
    }
    // Vidéo en fil
    {
        e.newProject();
        e.setCompositionSize(QSize(640, 360));
        int v2 = e.addLayer("v");
        CHECK(e.setLayerVideo(v2, root + "/media/h264.mp4", &err));
        CHECK(waitFrames(20));
        Engine::Lock lk(&e.mutex());
        CHECK(e.layer(v2)->sourceTex.w == 1280);
    }
    // Master : noir complet
    {
        e.newProject();
        int c = e.addLayer("blanc");
        e.setLayerIsf(c, root + "/../isf/generateurs/CouleurUnie.fs", &err);
        e.fadeMaster(1.0, 0);
        CHECK(waitFrames(4));
        QImage lit = e.grabOutput();
        CHECK(!lit.isNull() && qGray(lit.pixel(lit.width() / 2, lit.height() / 2)) > 240);
        e.fadeMaster(0.0, 0);
        CHECK(waitFrames(4));
        QImage dark = e.grabOutput();
        CHECK(!dark.isNull() && qGray(dark.pixel(dark.width() / 2, dark.height() / 2)) < 2);
        e.fadeMaster(1.0, 1.0); // remontée en 1 s
        QThread::msleep(500);
        const double mid = e.masterLevel();
        std::printf("       niveau du master à mi-fondu : %.2f\n", mid);
        CHECK(mid > 0.25 && mid < 0.75);
    }
    // 6. Publication : relecture GPU -> fil d'envoi (BGRA, lignes de haut en bas)
    {
        e.newProject();
        e.fadeMaster(1.0, 0);
        e.setCompositionSize(QSize(64, 32));
        QImage img(64, 32, QImage::Format_RGBA8888);
        img.fill(QColor(255, 0, 0));
        for (int y = 16; y < 32; ++y)
            for (int x = 0; x < 64; ++x) img.setPixelColor(x, y, QColor(0, 0, 255));
        img.save(tmp + "/hautrouge.png");
        int li = e.addLayer("img");
        CHECK(e.setLayerImage(li, tmp + "/hautrouge.png", &err));
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
        std::printf("       %d images reçues par le fil d'envoi\n", frames);
        CHECK(frames >= 10);
        CHECK(last.width == 64 && last.height == 32 && last.stride == 256);
        auto px = [&](int x, int y) { const uint8_t *p = last.bgra.data() + y * last.stride + x * 4; return QColor(p[2], p[1], p[0]); };
        std::printf("       pixel haut %s, bas %s\n", qPrintable(px(10, 2).name()), qPrintable(px(10, 29).name()));
        CHECK(px(10, 2) == QColor(255, 0, 0));  // haut : rouge
        CHECK(px(10, 29) == QColor(0, 0, 255)); // bas : bleu
        CHECK(last.timestamp100ns > 0);
    }
    e.setTestTap(nullptr);
    {
        // Réglages : désactivé par défaut, Syphon/Spout signalés selon la plateforme, aller-retour JSON
        PublishSettings ps;
        ps[PublishKind::Ndi].enabled = true;
        ps[PublishKind::Ndi].name = "Scène";
        ps[PublishKind::Syphon].enabled = true;
        ps.omtQuality = 50;
        CHECK(PublishSettings::fromJson(ps.toJson()) == ps);
        e.setPublishSettings(ps);
        CHECK(waitFrames(3));
        const PublishState syphon = e.publishState(PublishKind::Syphon);
        std::printf("       Syphon : %s\n       NDI : %s\n", qPrintable(syphon.text), qPrintable(e.publishState(PublishKind::Ndi).text));
        CHECK(publishCompiledIn(PublishKind::Syphon) ? syphon.level != PublishState::Unavailable
                                                     : syphon.level == PublishState::Unavailable);
        CHECK(e.publishState(PublishKind::Omt).level == PublishState::Off);
        e.setPublishSettings(PublishSettings());
        CHECK(waitFrames(3));
        CHECK(e.publishState(PublishKind::Ndi).level == PublishState::Off);
    }

    // 7. Médias : usage, fichier introuvable conservé, remplacement, chutier
    {
        e.newProject();
        QFile::copy(root + "/media/h264.mp4", tmp + "/clip.mp4");
        int v = e.addLayer("Clip");
        CHECK(e.setLayerVideo(v, tmp + "/clip.mp4", &err));
        int g = e.addLayer("Masque");
        e.setLayerIsf(g, root + "/../isf/generateurs/Mire.fs", &err);
        int fx = e.addEffect(g, root + "/../isf/effets/Masque.fs", &err);
        CHECK(fx == 0);
        {
            IsfInstance *inst;
            {
                Engine::Lock lk(&e.mutex());
                inst = e.layer(g)->effects[0].get();
            }
            CHECK(e.setIsfImageInput(inst, 1, tmp + "/hautrouge.png", &err));
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
        const Engine::MediaRef *mask = find(tmp + "/hautrouge.png");
        CHECK(mask && !mask->video && mask->users.size() == 1 && mask->users[0].contains("Masque"));
        CHECK(e.layersUsingMedia(tmp + "/hautrouge.png") == QList<int>{g});

        // Enregistrer, supprimer la vidéo, rouvrir : le chemin est conservé et signalé
        CHECK(e.saveProject(tmp + "/media.lanterne", {}, &err));
        QFile::rename(tmp + "/clip.mp4", tmp + "/clip_deplace.mp4");
        QString warn;
        CHECK(e.loadProject(tmp + "/media.lanterne", nullptr, &warn));
        std::printf("       avertissement : %s\n", qPrintable(warn));
        CHECK(warn.contains("introuvable"));
        int vi = -1;
        for (int i = 0; i < e.layerCount(); ++i) if (e.layer(i)->name == "Clip") vi = i;
        CHECK(vi >= 0);
        usage = e.mediaUsage();
        clip = find(tmp + "/clip.mp4");
        CHECK(clip && clip->missing && clip->video);
        CHECK(e.layerJson(vi).value("source").toObject().value("type").toString() == "video");
        CHECK(e.binItems().size() == 2);
        // Remplacement : la vidéo revient, le mapping est conservé
        Mapping before = e.layer(vi)->mapping;
        CHECK(e.relinkLayerMedia(vi, tmp + "/clip.mp4", tmp + "/clip_deplace.mp4", &err));
        e.relinkBinItem(tmp + "/clip.mp4", tmp + "/clip_deplace.mp4");
        CHECK(e.layer(vi)->type == SourceType::Video && e.layer(vi)->error.isEmpty());
        CHECK(e.layer(vi)->mapping.toJson() == before.toJson());
        CHECK(e.binItems().contains(tmp + "/clip_deplace.mp4"));
        QFile::remove(tmp + "/clip_deplace.mp4");
        VideoDecoder::Info info;
        CHECK(VideoDecoder::probe(root + "/media/prores.mov", &info) && info.width == 1920 && info.codec == "prores");
    }

    e.stop();
    CHECK(!e.isThreaded());
    e.renderFrame(); // retour au mode manuel
    CHECK(!e.grabOutput().isNull());

    std::printf("\n%s (%d échec(s))\n", failures ? "ÉCHEC" : "TOUT EST OK", failures);
    e.shutdown();
    return failures ? 1 : 0;
}
