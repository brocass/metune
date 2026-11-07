#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "theme.h"
#include "translator.h"

class MetuneTests : public QObject {
    Q_OBJECT

private slots:
    void parseStripsOfficialSuffix()
    {
        const ParsedName p = Backend::parseFilename("Evanescence - My Immortal (Official HD Music Video).mp4");
        QCOMPARE(p.artist, QString("Evanescence"));
        QCOMPARE(p.title, QString("My Immortal"));
    }

    void parseKeepsParentheticalTitle()
    {
        const ParsedName p = Backend::parseFilename("A Flock Of Seagulls - I Ran (So Far Away).mp4");
        QCOMPARE(p.artist, QString("A Flock Of Seagulls"));
        QCOMPARE(p.title, QString("I Ran (So Far Away)"));
    }

    void parseHandlesLyricsAndAudio()
    {
        const ParsedName p = Backend::parseFilename("ABBA - Dancing Queen [Lyrics].mp4");
        QCOMPARE(p.title, QString("Dancing Queen"));
        const ParsedName q = Backend::parseFilename("ABBA - Knowing Me, Knowing You (Official Audio).mp4");
        QCOMPARE(q.title, QString("Knowing Me, Knowing You"));
    }

    void cleanedNameRoundTrips()
    {
        const QString name = Backend::cleanedFileName("Evanescence", "My Immortal (Official HD Music Video)", ".mp4");
        QCOMPARE(name, QString("Evanescence - My Immortal.mp4"));
    }

    void decadeFromYearMaps()
    {
        QCOMPARE(Backend::decadeForYear(2016), QString("2010"));
        QCOMPARE(Backend::decadeForYear(1998), QString("1990"));
        QCOMPARE(Backend::decadeForYear(2003), QString("2000"));
        QVERIFY(Backend::decadeForYear(1800).isEmpty());
    }

    void yearFromDateStringParses()
    {
        QCOMPARE(Backend::yearFromDateString("2003-11-07"), 2003);
        QCOMPARE(Backend::yearFromDateString("1998"), 1998);
        QCOMPARE(Backend::yearFromDateString(""), 0);
    }

    void excludedPathsSkipMetubeDir()
    {
        QVERIFY(Backend::isExcludedPath("/mnt/pool/USENET/MeTube/.metube/completed.json"));
        QVERIFY(Backend::isExcludedPath("/mnt/pool/USENET/MeTube/NEWMUSIC/video.mp4.part"));
        QVERIFY(!Backend::isExcludedPath("/mnt/pool/USENET/MeTube/NEWMUSIC/video.mp4"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(MetuneTests)
#include "metune_tests.moc"
