#include "Engine.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QPalette>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>

static void applyDarkTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor base(30, 30, 33), window(40, 40, 44), text(225, 225, 228), accent(255, 150, 40);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(52, 52, 57));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::PlaceholderText, QColor(130, 130, 135));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 125));
    app.setPalette(p);
    app.setStyleSheet("QGroupBox { font-weight: bold; border: 1px solid #4a4a50; border-radius: 4px; margin-top: 10px; "
                      "padding-top: 8px; } "
                      "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #ff9628; }");
}

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
    applyDarkTheme(app);

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
        for (int k = 0; k < kPublishKindCount; ++k) {
            const PublishState st = engine.publishState(PublishKind(k));
            if (st.level != PublishState::Off)
                qInfo("Publishing %s: %s (receivers: %d)", qPrintable(publishKindName(PublishKind(k))), qPrintable(st.text),
                      st.receivers);
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
        else w.offerRecovery();
        if (!project.isEmpty()) w.openProject(project);
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
