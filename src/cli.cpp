#include "cli.h"

#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QUrl>
#include <iostream>

void Cli::printHelp()
{
    std::cout << "metune — clean MeTube music videos for Emby.\n"
              << "\nUsage:\n"
              << "  metune open                      Open the graphical app\n"
              << "  metune scan <dir> [--json]       List videos in a folder\n"
              << "  metune lookup \"Artist - Song\" [--json]\n"
              << "                                     Find release year + decade via MusicBrainz\n"
              << "  metune process <file|dir> [--dest <dir>] [--no-normalize] [--no-crop]\n"
              << "                                   Clean, tag, normalize (-14 LUFS),\n"
              << "                                   crop black bars, save to dest\n"
              << "  metune skill install             Install the agent skill\n"
              << "\nAll commands exit 0 on success, 1 on error (details on stderr).\n"
              << "Originals are never modified; --remove-original deletes the\n"
              << "original only after the processed file succeeds.\n";
}

static void printJson(const QJsonObject &obj)
{
    std::cout << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)).toStdString() << "\n";
}

static QStringList collectVideoFiles(const QString &path)
{
    QStringList files;
    const QFileInfo info(path);
    if (!info.exists()) {
        std::cerr << "No such file or folder: " << path.toStdString() << "\n";
        return files;
    }
    if (info.isFile()) {
        if (!Backend::isExcludedPath(path))
            files << info.absoluteFilePath();
        return files;
    }
    if (info.isDir()) {
        const QStringList exts = Backend::supportedExtensions();
        QDirIterator it(info.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            if (Backend::isExcludedPath(f))
                continue;
            const QString ext = QFileInfo(f).suffix().prepend('.').toLower();
            if (exts.contains(ext))
                files << f;
        }
        std::sort(files.begin(), files.end());
    }
    return files;
}

int Cli::run(const QStringList &args)
{
    const QString cmd = args.value(1);

    bool json = args.contains("--json");
    QString dest = args.contains("--dest") ? args.value(args.indexOf("--dest") + 1)
                                           : "/mnt/pool/Videos/Music Videos/MeTube";
    bool noNormalize = args.contains("--no-normalize");
    bool noCrop = args.contains("--no-crop");
    bool removeOriginal = args.contains("--remove-original");

    // Strip flags to find positionals.
    QStringList pos;
    for (int i = 2; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (!a.startsWith("--") && !(i > 0 && args.at(i - 1) == "--dest"))
            pos << a;
    }

    if (cmd == "scan") {
        if (pos.isEmpty()) {
            std::cerr << "scan needs a folder (or file).\n";
            return 1;
        }
        const QStringList files = collectVideoFiles(pos.first());
        if (files.isEmpty()) {
            std::cerr << "No videos found.\n";
            return 1;
        }
        if (json) {
            QJsonArray arr;
            for (const QString &f : files) {
                const ParsedName parsed = Backend::parseFilename(QFileInfo(f).fileName());
                QJsonObject o;
                o["path"] = f;
                o["fileName"] = QFileInfo(f).fileName();
                o["artist"] = parsed.artist;
                o["title"] = parsed.title;
                arr.append(o);
            }
            printJson({{"count", arr.size()}, {"videos", arr}});
        } else {
            for (const QString &f : files) {
                const ParsedName parsed = Backend::parseFilename(QFileInfo(f).fileName());
                std::cout << QFileInfo(f).fileName().toStdString() << "  →  "
                          << parsed.artist.toStdString() << " - "
                          << parsed.title.toStdString() << "\n";
            }
        }
        return 0;
    }

    if (cmd == "lookup") {
        if (pos.isEmpty()) {
            std::cerr << "lookup needs \"Artist - Song\".\n";
            return 1;
        }
        const QString input = pos.join(" ");
        const int dash = input.indexOf(" - ");
        QString artist, title;
        if (dash >= 2) {
            artist = input.left(dash).trimmed();
            title = Backend::cleanTitle(input.mid(dash + 3).trimmed());
        } else {
            title = Backend::cleanTitle(input.trimmed());
        }
        // Quick synchronous MusicBrainz via curl (Qt network needs event loop;
        // curl keeps the CLI self-contained and scriptable).
        QString url = "https://musicbrainz.org/ws/2/recording/?query="
            + QUrl::toPercentEncoding(QString("recording:\"%1\" AND artist:\"%2\"").arg(title, artist))
            + "&fmt=json&limit=5";
        QProcess curl;
        curl.start("curl", {"-sS", "-A", "metune/0.1.0", url});
        if (!curl.waitForFinished(15000)) {
            std::cerr << "Lookup timed out (offline?).\n";
            return 1;
        }
        if (curl.exitCode() != 0) {
            std::cerr << "Lookup failed: " << curl.readAllStandardError().constData() << "\n";
            return 1;
        }
        const QJsonObject root = QJsonDocument::fromJson(curl.readAllStandardOutput()).object();
        const QJsonArray recs = root.value("recordings").toArray();
        QString date;
        for (const auto &v : recs) {
            const QJsonObject rec = v.toObject();
            const QString d = rec.value("first-release-date").toString();
            if (!d.isEmpty()) { date = d; break; }
            const QJsonArray rels = rec.value("releases").toArray();
            if (!rels.isEmpty()) {
                date = rels.first().toObject().value("date").toString();
                if (!date.isEmpty()) break;
            }
        }
        const int year = Backend::yearFromDateString(date);
        const QString decade = Backend::decadeForYear(year);
        if (year <= 0) {
            std::cerr << "No release date found for " << artist.toStdString() << " - " << title.toStdString() << "\n";
            return 1;
        }
        if (json) {
            printJson({{"artist", artist}, {"title", title},
                       {"date", date}, {"year", year}, {"genre", decade}});
        } else {
            std::cout << artist.toStdString() << " - " << title.toStdString()
                      << " → " << year << " (" << decade.toStdString() << ")\n";
        }
        return 0;
    }

    if (cmd == "process") {
        if (pos.isEmpty()) {
            std::cerr << "process needs a file or folder.\n";
            return 1;
        }
        const QStringList files = collectVideoFiles(pos.first());
        if (files.isEmpty()) {
            std::cerr << "No videos found to process.\n";
            return 1;
        }
        // Run the pipeline synchronously via ffmpeg/ffprobe (same stages as
        // the GUI, minus the GUI). Originals are kept unless --remove-original.
        QStringList results;
        int failed = 0;
        for (const QString &file : files) {
            const ParsedName parsed = Backend::parseFilename(QFileInfo(file).fileName());
            const QString cleanName = Backend::cleanedFileName(parsed.artist, parsed.title,
                QFileInfo(file).suffix());
            QDir().mkpath(dest);
            const QString tmpDir = dest + "/.metune-tmp";
            QDir().mkpath(tmpDir);
            const QString tmpOutput = tmpDir + "/" + cleanName;
            QString finalOutput = dest + "/" + cleanName;
            if (QFileInfo::exists(finalOutput)) {
                const QFileInfo ci(cleanName);
                int n = 2;
                while (QFileInfo::exists(dest + "/" + QString("%1 (%2).%3").arg(ci.baseName()).arg(n).arg(ci.suffix())))
                    ++n;
                finalOutput = dest + "/" + QString("%1 (%2).%3").arg(ci.baseName()).arg(n).arg(ci.suffix());
            }

            // Year lookup (best effort; offline just leaves genre blank).
            QString yearStr, genreStr;
            {
                const QString url = "https://musicbrainz.org/ws/2/recording/?query="
                    + QUrl::toPercentEncoding(QString("recording:\"%1\" AND artist:\"%2\"").arg(parsed.title, parsed.artist))
                    + "&fmt=json&limit=5";
                QProcess curl;
                curl.start("curl", {"-sS", "-A", "metune/0.1.0", url});
                if (curl.waitForFinished(15000) && curl.exitCode() == 0) {
                    const QJsonObject root = QJsonDocument::fromJson(curl.readAllStandardOutput()).object();
                    const QJsonArray recs = root.value("recordings").toArray();
                    QString date;
                    for (const auto &v : recs) {
                        const QJsonObject rec = v.toObject();
                        const QString d = rec.value("first-release-date").toString();
                        if (!d.isEmpty()) { date = d; break; }
                        const QJsonArray rels = rec.value("releases").toArray();
                        if (!rels.isEmpty()) {
                            date = rels.first().toObject().value("date").toString();
                            if (!date.isEmpty()) break;
                        }
                    }
                    const int y = Backend::yearFromDateString(date);
                    if (y > 0) {
                        yearStr = QString::number(y);
                        genreStr = Backend::decadeForYear(y);
                    }
                }
            }

            // 1. Crop detect (optional)
            QString cropString;
            if (!noCrop) {
                QProcess p;
                p.start("ffmpeg", {"-hide_banner", "-y", "-ss", "10", "-t", "30",
                                   "-i", file, "-vf", "cropdetect=limit=24:round=2",
                                   "-f", "null", "-"});
                p.waitForFinished(120000);
                const QString err = p.readAllStandardError();
                static const auto re = QRegularExpression(R"re(crop=(\d+):(\d+):(\d+):(\d+))re");
                auto it = re.globalMatch(err);
                QString last;
                int cw = 0, ch = 0;
                while (it.hasNext()) {
                    const auto m = it.next();
                    last = m.captured(0);
                    cw = m.captured(1).toInt();
                    ch = m.captured(2).toInt();
                }
                if (!last.isEmpty()) {
                    // Full-frame crop means no bars — skip re-encode.
                    QProcess probe;
                    probe.start("ffprobe", {"-v", "error", "-select_streams", "v:0",
                                            "-show_entries", "stream=width,height",
                                            "-of", "csv=p=0", file});
                    bool full = false;
                    if (probe.waitForFinished(10000)) {
                        const QString wh = probe.readAllStandardOutput().trimmed();
                        const int ow = wh.section(',', 0, 0).toInt();
                        const int oh = wh.section(',', 1, 1).toInt();
                        if (ow > 0 && oh > 0 && cw >= ow && ch >= oh)
                            full = true;
                    }
                    if (!full)
                        cropString = last;
                }
            }

            // 2. Loudnorm measure (optional)
            QJsonObject measured;
            QString measuredJson;
            if (!noNormalize) {
                QProcess p;
                p.start("ffmpeg", {"-hide_banner", "-i", file, "-vn",
                                   "-af", "loudnorm=I=-14:TP=-1.5:LRA=11:print_format=json",
                                   "-f", "null", "-"});
                if (!p.waitForFinished(600000)) {
                    std::cerr << "Measure timed out: " << file.toStdString() << "\n";
                    ++failed;
                    continue;
                }
                const QString err = p.readAllStandardError();
                const int start = err.lastIndexOf('{');
                if (start >= 0)
                    measured = QJsonDocument::fromJson(err.mid(start).toUtf8()).object();
                measuredJson = err.mid(start);
            }

            // 3. Final encode.
            QStringList args2;
            args2 << "-hide_banner" << "-y" << "-i" << file;
            bool reencodeVideo = !cropString.isEmpty();
            if (!cropString.isEmpty())
                args2 << "-vf" << cropString;
            if (!noNormalize && !measured.isEmpty()) {
                auto get = [&](const QString &k) { return measured.value(k).toString("0"); };
                const QString filter = QString("loudnorm=I=-14:TP=-1.5:LRA=11:measured_I=%1:measured_TP=%2:measured_LRA=%3:measured_thresh=%4:offset=%5:linear=true:print_format=json")
                    .arg(get("input_i"), get("input_tp"), get("input_lra"), get("input_thresh"), get("target_offset"));
                args2 << "-af" << filter << "-c:a" << "aac" << "-b:a" << "192k";
                if (!reencodeVideo) args2 << "-c:v" << "copy";
            } else if (!noNormalize) {
                args2 << "-af" << "loudnorm=I=-14:TP=-1.5:LRA=11" << "-c:a" << "aac" << "-b:a" << "192k";
                if (!reencodeVideo) args2 << "-c:v" << "copy";
            } else {
                args2 << "-c:a" << "copy";
                if (!reencodeVideo) args2 << "-c:v" << "copy";
            }
            if (reencodeVideo)
                args2 << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "23";
            if (!parsed.artist.isEmpty()) args2 << "-metadata" << QString("artist=%1").arg(parsed.artist);
            if (!parsed.title.isEmpty()) args2 << "-metadata" << QString("title=%1").arg(parsed.title);
            if (!yearStr.isEmpty()) args2 << "-metadata" << QString("date=%1").arg(yearStr);
            if (!genreStr.isEmpty()) args2 << "-metadata" << QString("genre=%1").arg(genreStr);
            args2 << "-metadata" << "comment=metune" << "-map_metadata" << "-1" << tmpOutput;

            QProcess enc;
            enc.start("ffmpeg", args2);
            if (!enc.waitForFinished(0)) {
                enc.waitForFinished(-1);
            }
            if (enc.exitCode() != 0 || !QFileInfo::exists(tmpOutput)) {
                std::cerr << "Processing failed for " << file.toStdString() << "\n"
                          << enc.readAllStandardError().right(800).constData() << "\n";
                ++failed;
                continue;
            }
            if (!QFile::rename(tmpOutput, finalOutput)) {
                std::cerr << "Could not move " << tmpOutput.toStdString() << " into place\n";
                ++failed;
                continue;
            }
            results << finalOutput;
            if (removeOriginal)
                QFile::remove(file);
        }

        if (json) {
            QJsonArray arr;
            for (const QString &r : results) arr.append(r);
            printJson({{"processed", arr.size()}, {"failed", failed}, {"outputs", arr}});
        } else {
            std::cout << "Processed " << results.size() << " file(s), " << failed << " failed.\n";
            for (const QString &r : results)
                std::cout << "  " << r.toStdString() << "\n";
        }
        return failed == 0 ? 0 : 1;
    }

    if (cmd == "skill" && args.value(2) == "install") {
        std::cerr << "Agent skill is bundled; see README for the install path.\n";
        return 1;
    }

    printHelp();
    return 1;
}
