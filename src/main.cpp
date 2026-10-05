#include "Engine.h"
#include "MainWindow.h"
#include "Widgets.h"

#include <QFileInfo>
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>

int main(int argc, char *argv[])
{
    // OpenGL 3.3 core everywhere (macOS then provides 4.1 core). Contexts shared between engine and windows.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(8);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    QApplication::setApplicationName("Fulskrin");
    QApplication::setOrganizationName("Fulskrin");
    QApplication::setApplicationVersion("0.1.0");
    theme::applyToApplication(app);

    QCommandLineParser cli;
    cli.setApplicationDescription("Fulskrin — multi-layer video mapping, FFmpeg playback, ISF shaders");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption renderOpt("render", "Render the project headless and save the output as PNG.", "output.png");
    QCommandLineOption framesOpt("frames", "Number of frames to render before capture (with --render).", "n", "30");
    QCommandLineOption shotOpt("screenshot", "Capture the main window, then quit (tests).", "capture.png");
    cli.addOption(renderOpt);
    cli.addOption(framesOpt);
    cli.addOption(shotOpt);
    cli.addPositionalArgument("project", ".fulskrin project to open");
    cli.process(app);
    const QString project = cli.positionalArguments().value(0);

    Engine engine;
    QString err;
    if (!engine.initialize(&err)) {
        if (cli.isSet(renderOpt)) {
            qCritical("%s", qPrintable(err));
            return 1;
        }
        QMessageBox::critical(nullptr, "Fulskrin", QStringLiteral("Could not initialize OpenGL:\n") + err);
        return 1;
    }

    // Headless mode: render N frames and write the output (automated tests, project checking).
    if (cli.isSet(renderOpt)) {
        if (!project.isEmpty()) {
            QString perr;
            engine.loadProject(project, nullptr, &perr);
            if (!perr.isEmpty()) qWarning("%s", qPrintable(perr));
        }
        const int frames = qMax(1, cli.value(framesOpt).toInt());
        for (int i = 0; i < frames; ++i) {
            engine.renderFrame();
            QThread::msleep(16);
        }
        for (int i = 0; i < engine.layerCount(); ++i) {
            Layer *l = engine.layer(i);
            if (!l->error.isEmpty()) qWarning("Layer %s: %s", qPrintable(l->name), qPrintable(l->error));
            for (auto &fx : l->effects)
                if (!fx->error().isEmpty()) qWarning("Effect %s: %s", qPrintable(fx->name()), qPrintable(fx->error()));
        }
        for (int v : engine.viewports()) {
            const quint64 id = engine.layerId(v);
            for (int k = 0; k < kPublishKindCount; ++k) {
                const PublishState st = engine.publishState(id, PublishKind(k));
                if (st.level != PublishState::Off)
                    qInfo("Publishing %s %s: %s (receivers: %d)", qPrintable(engine.layer(v)->name),
                          qPrintable(publishKindName(PublishKind(k))), qPrintable(st.text), st.receivers);
            }
        }
        const bool ok = engine.grabOutput().save(cli.value(renderOpt));
        engine.shutdown();
        return ok ? 0 : 2;
    }

    // GUI: rendering runs in its own thread, independent of the UI.
    engine.start();

    int rc;
    {
        MainWindow w(&engine);
        w.show();
        if (cli.isSet(shotOpt)) {
            w.setAutosaveEnabled(false);
            w.setQuiet(true);
        }
        const bool restored = !cli.isSet(shotOpt) && w.offerRecovery();
        // A project given to open after a session was restored: the restored session is not dropped silently —
        // the same project stays as restored; another one asks first whether to save the session
        if (!project.isEmpty()) {
            const auto same = [](const QString &a, const QString &b) {
                const QString ca = QFileInfo(a).canonicalFilePath(), cb = QFileInfo(b).canonicalFilePath();
                return !ca.isEmpty() && ca == cb;
            };
            if (!restored) w.openProject(project);
            else if (!same(project, engine.projectPath()) && w.maybeSave()) w.openProject(project);
        }
        if (cli.isSet(shotOpt)) {
            QTimer::singleShot(2500, &w, [&w, &cli, &shotOpt] {
                w.grab().save(cli.value(shotOpt));
                QCoreApplication::exit(0); // no "Save?" dialog
            });
        }
        rc = app.exec();
    }
    engine.shutdown();
    return rc;
}
