// Tests du moteur sans interface : chargement/sauvegarde, vidéo, ISF (multi-passes, .vs, erreurs).
#include "Engine.h"
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
    std::printf("\n%s (%d échec(s))\n", failures ? "ÉCHEC" : "TOUT EST OK", failures);
    e.shutdown();
    return failures ? 1 : 0;
}
