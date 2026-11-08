// metune — clean MeTube music videos for Emby, from one watch folder.
// Strips "(Official HD Music Video)"-style suffixes, tags Artist/Title,
// sets Genre to the release decade (via MusicBrainz), normalizes audio
// to -14 LUFS (ffmpeg loudnorm, the Shutter Encoder equivalent), crops
// black bars (ffmpeg cropdetect, the HandBrake-preset equivalent), and
// saves a fresh copy to the Emby folder. Originals are never modified.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[])
{
    // CLI subcommands run headless, with no display. The GUI is behind
    // `metune open` (or no arguments at all).
    if (argc >= 2 && QString(argv[1]) != "open") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QCoreApplication app(argc, argv);
        app.setApplicationName("metune");
        QStringList flat;
        for (int i = 0; i < argc; ++i)
            flat << argv[i];
        return Cli::run(flat);
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("metune");
    app.setApplicationVersion("0.1.1");

    // Matches the window to metune.desktop, so the compositor and launcher
    // pick up the installed icon.
    app.setDesktopFileName("metune");
    app.setWindowIcon(QIcon::fromTheme("metune"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
