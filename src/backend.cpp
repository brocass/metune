#include "backend.h"
#include "portalfilepicker.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QDesktopServices>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

// ---------------------------------------------------------------------------
// Parsing helpers (pure, unit-tested below).
// ---------------------------------------------------------------------------

QStringList Backend::supportedExtensions()
{
    return {".mp4", ".mkv", ".webm", ".mov"};
}

bool Backend::isExcludedPath(const QString &path)
{
    const QStringList parts = path.split('/');
    for (const QString &p : parts) {
        if (p == ".metube" || p.startsWith('.'))
            return true;
    }
    const QString lower = path.toLower();
    if (lower.endsWith(".part") || lower.endsWith(".ytdl") || lower.endsWith(".tmp"))
        return true;
    return false;
}

// Strip YouTube junk suffixes like "(Official HD Music Video)", "[Lyrics]",
// but keep meaningful parentheticals such as "(So Far Away)".
QString Backend::cleanTitle(const QString &title)
{
    QString t = title.trimmed();
    static const QStringList junkWords = {"official", "music video", "hd", "4k", "lyrics", "lyric",
                                          "audio", "video", "visualizer", "vevo", "remastered"};
    // Repeatedly strip a trailing "(…)" or "[…]" group if it looks like
    // release junk (contains a junk word) or is empty.
    while (true) {
        const QChar last = t.isEmpty() ? QChar() : t.back();
        if (last != ')' && last != ']')
            break;
        const int openParen = t.lastIndexOf('(');
        const int openBracket = t.lastIndexOf('[');
        int open = -1;
        if (last == ')' && openParen > openBracket)
            open = openParen;
        else if (last == ']' && openBracket > openParen)
            open = openBracket;
        if (open < 0)
            break;
        const QString inside = t.mid(open + 1, t.size() - open - 2).trimmed();
        if (inside.isEmpty()) {
            t = t.left(open).trimmed();
            continue;
        }
        const QString lowered = inside.toLower();
        bool junk = false;
        for (const QString &w : junkWords) {
            // Whole-word match so "So Far Away" is kept while
            // "Official HD Music Video" is stripped.
            int at = lowered.indexOf(w);
            while (at >= 0) {
                const bool leftOk = at == 0 || !lowered.at(at - 1).isLetterOrNumber();
                const bool rightOk = at + w.size() >= lowered.size()
                    || !lowered.at(at + w.size()).isLetterOrNumber();
                if (leftOk && rightOk) {
                    junk = true;
                    break;
                }
                at = lowered.indexOf(w, at + 1);
            }
            if (junk)
                break;
        }
        if (!junk)
            break;
        t = t.left(open).trimmed();
    }
    // Collapse whitespace runs to a single space.
    const QStringList words = t.split(QRegularExpression(R"re(\s+)re"), Qt::SkipEmptyParts);
    return words.join(' ');
}

ParsedName Backend::parseFilename(const QString &fileName)
{
    const QFileInfo info(fileName);
    QString base = info.baseName().trimmed();
    ParsedName out;
    // Split on " - " (most MeTube names) once.
    const int dash = base.indexOf(" - ");
    if (dash >= 4) {
        out.artist = base.left(dash).trimmed();
        out.title = cleanTitle(base.mid(dash + 3).trimmed());
    } else {
        out.artist.clear();
        out.title = cleanTitle(base);
    }
    // Clean up dotted/underscored separators.
    out.artist.replace('_', ' ');
    out.title.replace('_', ' ');
    out.artist = out.artist.trimmed();
    out.title = out.title.trimmed();
    return out;
}

QString Backend::cleanedFileName(const QString &artist, const QString &title, const QString &ext)
{
    QString cleanExt = ext.toLower();
    if (!cleanExt.startsWith('.'))
        cleanExt.prepend('.');
    if (cleanExt.isEmpty())
        cleanExt = ".mp4";
    QString a = artist.trimmed();
    QString t = cleanTitle(title.trimmed());
    if (!a.isEmpty() && !t.isEmpty())
        return a + " - " + t + cleanExt;
    if (!t.isEmpty())
        return t + cleanExt;
    if (!a.isEmpty())
        return a + cleanExt;
    return QString();
}

QString Backend::decadeForYear(int year)
{
    if (year < 1900 || year > 2099)
        return {};
    return QString::number((year / 10) * 10) + "'s";
}

int Backend::yearFromDateString(const QString &date)
{
    static const QRegularExpression re(R"re((19|20)\d{2})re");
    const auto m = re.match(date.trimmed());
    if (m.hasMatch())
        return m.captured(0).toInt();
    return 0;
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

Backend::Backend(QObject *parent)
    : QObject(parent), m_net(this)
{
    m_picker = new PortalFilePicker(this);
    connect(m_picker, &FilePicker::picked, this, &Backend::onPickerPicked);
    connect(m_picker, &FilePicker::failed, this, [this](const QString &reason) {
        setStatus(reason);
        emit pickerFailed(reason);
        appendLog(reason);
    });

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(1500);
    connect(&m_debounce, &QTimer::timeout, this, &Backend::onDebounceTimeout);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });

    connect(&m_process, &QProcess::readyReadStandardError, this, &Backend::onProcessReadyRead);
    connect(&m_process, &QProcess::finished, this, &Backend::onProcessFinished);

    connect(&m_net, &QNetworkAccessManager::finished, this, &Backend::onLookupFinished);

    loadSettings();
    if (!m_watchDir.isEmpty())
        collectFiles();
}

Backend::~Backend()
{
    if (m_lookupReply)
        m_lookupReply->abort();
}

void Backend::loadSettings()
{
    QSettings s("metune", "metune");
    m_watchDir = s.value("watchDir", "/mnt/pool/USENET/MeTube").toString();
    m_destDir = s.value("destDir", "/mnt/pool/Videos/Music Videos/MeTube").toString();
    m_normalize = s.value("normalize", true).toBool();
    m_cropBars = s.value("cropBars", true).toBool();
    m_autoProcess = s.value("autoProcess", false).toBool();
    m_removeOriginal = s.value("removeOriginal", false).toBool();
}

void Backend::saveSettings()
{
    QSettings s("metune", "metune");
    s.setValue("watchDir", m_watchDir);
    s.setValue("destDir", m_destDir);
    s.setValue("normalize", m_normalize);
    s.setValue("cropBars", m_cropBars);
    s.setValue("autoProcess", m_autoProcess);
    s.setValue("removeOriginal", m_removeOriginal);
}

void Backend::setStatus(const QString &s)
{
    m_statusText = s;
    emit statusChanged();
}

void Backend::setBusy(bool b)
{
    if (m_busy == b)
        return;
    m_busy = b;
    emit busyChanged();
}

void Backend::appendLog(const QString &s)
{
    m_log.append(s);
    emit logLine(s);
}

void Backend::rearmWatcher()
{
    const auto paths = m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);
    if (m_watchDir.isEmpty())
        return;
    QDir dir(m_watchDir);
    if (!dir.exists())
        return;
    // Watch recursively, skipping .metube and hidden folders.
    QDirIterator it(m_watchDir, QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    m_watcher.addPath(m_watchDir);
    while (it.hasNext()) {
        const QString p = it.next();
        if (!isExcludedPath(p))
            m_watcher.addPath(p);
    }
}

void Backend::collectFiles()
{
    if (m_watchDir.isEmpty() || !QFileInfo::exists(m_watchDir)) {
        m_items.clear();
        emit videosChanged();
        return;
    }
    const QStringList exts = supportedExtensions();
    QSet<QString> seen;
    QList<VideoItem> fresh;

    QDirIterator it(m_watchDir, exts, QDir::Files, QDirIterator::Subdirectories);
    // QDirIterator with filters needs a manual filter loop for extensions
    // including .mp4 etc. Re-collect with explicit loop below for safety.
    // (The above iterator with extensions is used as a start; we redo properly.)
    fresh.clear();
    seen.clear();
    QDirIterator it2(m_watchDir, QDir::Files, QDirIterator::Subdirectories);
    while (it2.hasNext()) {
        const QString path = it2.next();
        if (isExcludedPath(path))
            continue;
        const QString ext = QFileInfo(path).suffix().prepend('.').toLower();
        if (!exts.contains(ext))
            continue;
        if (seen.contains(path))
            continue;
        seen.insert(path);
        VideoItem item;
        item.path = path;
        item.fileName = QFileInfo(path).fileName();
        // Preserve previously known metadata if the file is unchanged.
        const int prior = findIndexByPath(path);
        if (prior >= 0 && m_items.at(prior).path == path) {
            item.artist = m_items.at(prior).artist;
            item.title = m_items.at(prior).title;
            item.year = m_items.at(prior).year;
            item.genre = m_items.at(prior).genre;
            item.status = m_items.at(prior).status;
            item.progress = m_items.at(prior).progress;
            item.outputPath = m_items.at(prior).outputPath;
        } else {
            const ParsedName parsed = parseFilename(item.fileName);
            item.artist = parsed.artist;
            item.title = parsed.title;
            // A file whose cleaned name already exists in the destination
            // was processed before — mark it Done so auto-process doesn't
            // redo the whole folder after a restart.
            const QString clean = cleanedFileName(item.artist, item.title, QFileInfo(path).suffix());
            if (!clean.isEmpty() && !m_destDir.isEmpty() && QFileInfo::exists(m_destDir + "/" + clean))
                item.status = "Done";
        }
        fresh.append(item);
    }
    std::sort(fresh.begin(), fresh.end(), [](const VideoItem &a, const VideoItem &b) {
        return a.fileName.compare(b.fileName, Qt::CaseInsensitive) < 0;
    });
    m_items = fresh;
    rearmWatcher();
    if (m_selectedIndex < 0 && !m_items.isEmpty()) {
        m_selectedIndex = 0;
        emit selectionChanged();
    } else if (m_selectedIndex >= m_items.size()) {
        m_selectedIndex = m_items.isEmpty() ? -1 : 0;
        emit selectionChanged();
    }
    emit videosChanged();
}

int Backend::findIndexByPath(const QString &path) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).path == path)
            return i;
    return -1;
}

QVariantList Backend::videos() const
{
    QVariantList list;
    for (const auto &it : m_items) {
        QVariantMap m;
        m["path"] = it.path;
        m["fileName"] = it.fileName;
        m["artist"] = it.artist;
        m["title"] = it.title;
        m["year"] = it.year;
        m["genre"] = it.genre;
        m["status"] = it.status;
        m["progress"] = it.progress;
        m["outputPath"] = it.outputPath;
        list.append(m);
    }
    return list;
}

void Backend::refreshVideoAt(int index)
{
    (void)index;
    emit videosChanged();
}

void Backend::setWatchDir(const QString &path)
{
    const QString clean = path.trimmed();
    if (clean == m_watchDir)
        return;
    m_watchDir = clean;
    saveSettings();
    emit settingsChanged();
    collectFiles();
    appendLog(tr("Watching %1").arg(m_watchDir));
}

void Backend::setDestDir(const QString &path)
{
    const QString clean = path.trimmed();
    if (clean == m_destDir)
        return;
    m_destDir = clean;
    saveSettings();
    emit settingsChanged();
    appendLog(tr("Destination %1").arg(m_destDir));
}

void Backend::setNormalize(bool v) { m_normalize = v; saveSettings(); emit settingsChanged(); }
void Backend::setCropBars(bool v) { m_cropBars = v; saveSettings(); emit settingsChanged(); }
void Backend::setAutoProcess(bool v) { m_autoProcess = v; saveSettings(); emit settingsChanged(); }
void Backend::setRemoveOriginal(bool v) { m_removeOriginal = v; saveSettings(); emit settingsChanged(); }

void Backend::pickWatchDir()
{
    m_pickingWatch = true;
    m_picker->pickDirectory(m_watchDir);
}

void Backend::pickDestDir()
{
    m_pickingWatch = false;
    m_picker->pickDirectory(m_destDir);
}

void Backend::onPickerPicked(const QString &path)
{
    if (path.isEmpty())
        return;
    if (m_pickingWatch)
        setWatchDir(path);
    else
        setDestDir(path);
}

void Backend::scan()
{
    collectFiles();
    setStatus(tr("%n video(s) found.", "", m_items.size()));
    appendLog(tr("Scanned %1 — %2 found.").arg(m_watchDir).arg(m_items.size()));
}

void Backend::select(int index)
{
    m_selectedIndex = index;
    emit selectionChanged();
    emit videosChanged();
}

void Backend::updateField(int index, const QString &field, const QString &value)
{
    if (index < 0 || index >= m_items.size())
        return;
    VideoItem &it = m_items[index];
    if (field == "artist") it.artist = value.trimmed();
    else if (field == "title") it.title = cleanTitle(value.trimmed());
    else if (field == "year") {
        it.year = value.trimmed();
        const int y = it.year.toInt();
        const QString decade = decadeForYear(y);
        if (!decade.isEmpty())
            it.genre = decade;
    }
    else if (field == "genre") it.genre = value.trimmed();
    else return;
    emit videosChanged();
}

void Backend::lookup(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    // One lookup at a time; process() calls this while already busy, so
    // "busy" must not block it (that stalled the pipeline with the app
    // permanently busy and no error shown).
    if (m_lookupReply)
        return;
    m_lookupIndex = index;
    const VideoItem it = m_items.at(index);
    if (it.artist.isEmpty() || it.title.isEmpty()) {
        setStatus(tr("Artist and title are needed for a year lookup."));
        return;
    }
    const QString query = QString("recording:\"%1\" AND artist:\"%2\"").arg(it.title, it.artist);
    QUrl url("https://musicbrainz.org/ws/2/recording/");
    QUrlQuery q;
    q.addQueryItem("query", query);
    q.addQueryItem("fmt", "json");
    q.addQueryItem("limit", "5");
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "metune/0.1.0 (Omarchy; https://musicbrainz.org/doc/MusicBrainz_API)");
    // A hanging request must not stall the pipeline — treat a timeout as a
    // failed lookup and carry on without a year.
    req.setTransferTimeout(15000);
    auto *reply = m_net.get(req);
    m_lookupReply = reply;
    setStatus(tr("Looking up %1 - %2…").arg(it.artist, it.title));
    m_items[index].status = "Looking up…";
    emit videosChanged();
}

void Backend::lookupSelected()
{
    lookup(m_selectedIndex);
}

void Backend::onLookupFinished(QNetworkReply *reply)
{
    m_lookupReply = nullptr;
    const int index = m_lookupIndex;
    m_lookupIndex = -1;
    reply->deleteLater();
    if (index < 0 || index >= m_items.size())
        return;
    VideoItem &it = m_items[index];
    if (reply->error() != QNetworkReply::NoError) {
        it.status = "Lookup failed";
        setStatus(tr("Year lookup failed (offline?) — genre left blank. You can type the year by hand."));
        appendLog(tr("Lookup failed for %1 - %2: %3").arg(it.artist, it.title, reply->errorString()));
        emit videosChanged();
        // If this lookup was part of an automated process, carry on.
        if (m_currentIndex == index && m_stage == "await-lookup")
            startCropDetect();
        return;
    }
    const QByteArray data = reply->readAll();
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    const QJsonArray recs = root.value("recordings").toArray();
    QString date;
    for (const auto &v : recs) {
        const QJsonObject rec = v.toObject();
        const QString d = rec.value("first-release-date").toString();
        if (!d.isEmpty()) {
            date = d;
            break;
        }
        const QJsonArray rels = rec.value("releases").toArray();
        if (!rels.isEmpty()) {
            date = rels.first().toObject().value("date").toString();
            if (!date.isEmpty())
                break;
        }
    }
    const int year = yearFromDateString(date);
    if (year > 0) {
        it.year = QString::number(year);
        it.genre = decadeForYear(year);
        it.status = "Pending";
        setStatus(tr("%1 - %2: %3 (%4)").arg(it.artist, it.title, it.year, it.genre));
        appendLog(tr("Found %1 - %2 → %3, genre %4").arg(it.artist, it.title, it.year, it.genre));
    } else {
        it.status = "Pending";
        setStatus(tr("No release date found for %1 - %2. Type the year by hand.").arg(it.artist, it.title));
        appendLog(tr("No MusicBrainz date for %1 - %2").arg(it.artist, it.title));
    }
    emit videosChanged();
    if (m_currentIndex == index && m_stage == "await-lookup")
        startCropDetect();
}

QString Backend::parsedArtist(const QString &fileName) const
{
    return parseFilename(fileName).artist;
}

QString Backend::parsedTitle(const QString &fileName) const
{
    return parseFilename(fileName).title;
}

QString Backend::lastStderrLine() const
{
    const auto lines = m_stderrTail.split('\n', Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines[i].trimmed();
        if (!line.isEmpty() && !line.startsWith("size="))
            return line.left(200);
    }
    return {};
}

QString Backend::uniqueOutputPath(const QString &cleanName) const
{
    QDir dest(m_destDir);
    if (dest.exists(cleanName))
        return {};
    QString candidate = dest.filePath(cleanName);
    if (!QFileInfo::exists(candidate))
        return candidate;
    const QFileInfo info(cleanName);
    const QString base = info.baseName();
    const QString ext = info.suffix().prepend('.');
    for (int n = 2; n < 100; ++n) {
        const QString name = QString("%1 (%2)%3").arg(base).arg(n).arg(ext);
        candidate = dest.filePath(name);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

void Backend::process(int index)
{
    if (index < 0 || index >= m_items.size() || m_busy)
        return;
    // Single shot: no queue entry, so completeCurrent() won't run it twice.
    m_queue.clear();
    setBusy(true);
    m_currentIndex = index;
    // If year is missing, try lookup first (async), then proceed. Files
    // without a parseable artist/title can't be looked up — process them
    // anyway rather than stalling.
    const VideoItem &it = m_items.at(index);
    if (it.year.isEmpty() && !it.artist.isEmpty() && !it.title.isEmpty()) {
        m_stage = "await-lookup";
        m_items[index].status = "Looking up…";
        emit videosChanged();
        lookup(index);
        return;
    }
    startCropDetect();
}

void Backend::processAll()
{
    if (m_busy)
        return;
    m_queue.clear();
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].status != "Done")
            m_queue.append(i);
    if (m_queue.isEmpty()) {
        setStatus(tr("Nothing to process."));
        return;
    }
    setBusy(true);
    startNextInQueue();
}

void Backend::stop()
{
    if (m_process.state() != QProcess::NotRunning)
        m_process.kill();
    if (m_lookupReply)
        m_lookupReply->abort();
    m_queue.clear();
    m_currentIndex = -1;
    setBusy(false);
    setStatus(tr("Stopped."));
    appendLog(tr("Stopped."));
    emit videosChanged();
}

void Backend::startNextInQueue()
{
    if (m_queue.isEmpty()) {
        setBusy(false);
        m_currentIndex = -1;
        setStatus(tr("All finished."));
        collectFiles();
        return;
    }
    m_currentIndex = m_queue.takeFirst();
    VideoItem &it = m_items[m_currentIndex];
    if (it.year.isEmpty() && !it.artist.isEmpty() && !it.title.isEmpty()) {
        m_stage = "await-lookup";
        it.status = "Looking up…";
        emit videosChanged();
        lookup(m_currentIndex);
        return;
    }
    startCropDetect();
}

void Backend::startCropDetect()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) {
        startNextInQueue();
        return;
    }
    VideoItem &it = m_items[m_currentIndex];
    m_stage = "crop";
    it.status = "Analyzing…";
    it.progress = 0;
    emit videosChanged();
    m_stderrTail.clear();
    m_cropString.clear();
    m_probeW = m_probeH = 0;
    if (m_cropBars) {
        // Sample 30s starting at 10s to keep it fast; music videos are short.
        m_process.start("ffmpeg", {"-hide_banner", "-y", "-ss", "10", "-t", "30",
                                   "-i", it.path, "-vf", "cropdetect=limit=24:round=2",
                                   "-f", "null", "-"});
        if (!m_process.waitForStarted(5000)) {
            completeCurrent(false, tr("Could not start ffmpeg — is it installed?"));
            return;
        }
    } else {
        startLoudnormMeasure();
    }
}

void Backend::startLoudnormMeasure()
{
    VideoItem &it = m_items[m_currentIndex];
    m_stage = "measure";
    it.status = "Measuring loudness…";
    emit videosChanged();
    m_stderrTail.clear();
    m_measuredJson.clear();
    if (m_normalize) {
        // Keep the whole output; loudnorm prints JSON to stderr at the end.
        m_process.start("ffmpeg", {"-hide_banner", "-i", it.path, "-vn",
                                   "-af", "loudnorm=I=-14:TP=-1.5:LRA=11:print_format=json",
                                   "-f", "null", "-"});
        if (!m_process.waitForStarted(5000)) {
            completeCurrent(false, tr("Could not start ffmpeg — is it installed?"));
            return;
        }
    } else {
        startFinalEncode();
    }
}

void Backend::startFinalEncode()
{
    VideoItem &it = m_items[m_currentIndex];
    const QString cleanName = cleanedFileName(it.artist, it.title, QFileInfo(it.path).suffix());
    if (cleanName.isEmpty()) {
        completeCurrent(false, tr("No usable filename."));
        return;
    }
    if (!QDir(m_destDir).exists()) {
        QDir().mkpath(m_destDir);
    }
    const QString dest = uniqueOutputPath(cleanName);
    if (dest.isEmpty()) {
        completeCurrent(false, tr("Destination folder is missing or full."));
        return;
    }
    m_finalOutput = dest;
    const QString tmpDir = m_destDir + "/.metune-tmp";
    QDir().mkpath(tmpDir);
    m_tmpOutput = tmpDir + "/" + cleanName;
    if (QFileInfo::exists(m_tmpOutput))
        QFile::remove(m_tmpOutput);

    QStringList args;
    args << "-hide_banner" << "-y" << "-i" << it.path;

    bool reencodeVideo = !m_cropString.isEmpty();
    if (!m_cropString.isEmpty())
        args << "-vf" << m_cropString;
    const QJsonObject measured = QJsonDocument::fromJson(m_measuredJson.toUtf8()).object();
    if (m_normalize && measured.contains("input_i")) {
        // Two-pass: reuse the measurements for a linear normalization.
        auto get = [&](const QString &key) { return measured.value(key).toString("0"); };
        const QString filter = QString("loudnorm=I=-14:TP=-1.5:LRA=11:measured_I=%1:measured_TP=%2:measured_LRA=%3:measured_thresh=%4:offset=%5:linear=true:print_format=json")
            .arg(get("input_i"), get("input_tp"), get("input_lra"), get("input_thresh"), get("target_offset"));
        args << "-af" << filter;
        args << "-c:a" << "aac" << "-b:a" << "192k";
        if (!reencodeVideo)
            args << "-c:v" << "copy";
    } else if (m_normalize) {
        // Measurement gave no usable JSON — single-pass dynamic mode.
        args << "-af" << "loudnorm=I=-14:TP=-1.5:LRA=11" << "-c:a" << "aac" << "-b:a" << "192k";
        if (!reencodeVideo)
            args << "-c:v" << "copy";
    } else {
        args << "-c:a" << "copy";
        if (!reencodeVideo)
            args << "-c:v" << "copy";
    }
    if (reencodeVideo)
        args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "23";

    if (!it.artist.isEmpty())
        args << "-metadata" << QString("artist=%1").arg(it.artist);
    if (!it.title.isEmpty())
        args << "-metadata" << QString("title=%1").arg(it.title);
    if (!it.year.isEmpty())
        args << "-metadata" << QString("date=%1").arg(it.year);
    if (!it.genre.isEmpty())
        args << "-metadata" << QString("genre=%1").arg(it.genre);
    args << "-metadata" << QString("comment=metune")
         << "-map_metadata" << "-1"
         << m_tmpOutput;

    m_stage = "encode";
    it.status = "Processing…";
    it.progress = 5;
    emit videosChanged();
    m_stderrTail.clear();
    m_duration = 0; // parsed from ffmpeg's own "Duration:" line below
    m_process.start("ffmpeg", args);
    if (!m_process.waitForStarted(5000))
        completeCurrent(false, tr("Could not start ffmpeg — is it installed?"));
}

void Backend::onProcessReadyRead()
{
    const QString text = QString::fromUtf8(m_process.readAllStandardError());
    // Keep a bounded tail so a failure can show ffmpeg's own error line.
    m_stderrTail += text;
    if (m_stderrTail.size() > 4000)
        m_stderrTail = m_stderrTail.right(4000);

    if (m_stage == "crop") {
        // cropdetect prints "crop=W:H:X:Y" lines; keep the last one. Its
        // "w:<W> h:<H>" output also tells us the source size, so a
        // full-frame crop needs no extra ffprobe run.
        static const QRegularExpression re(R"re(crop=(\d+):(\d+):(\d+):(\d+))re");
        auto all = re.globalMatch(text);
        while (all.hasNext()) {
            const auto m = all.next();
            const int w = m.captured(1).toInt();
            const int h = m.captured(2).toInt();
            if (w > 2 && h > 2) {
                m_cropString = QString("crop=%1:%2:%3:%4").arg(m.captured(1), m.captured(2), m.captured(3), m.captured(4));
                m_cropW = w;
                m_cropH = h;
            }
        }
        static const QRegularExpression reWH(R"re(\bw:(\d+) h:(\d+)\b)re");
        auto sizes = reWH.globalMatch(text);
        while (sizes.hasNext()) {
            const auto m = sizes.next();
            m_probeW = m.captured(1).toInt();
            m_probeH = m.captured(2).toInt();
        }
    } else if (m_stage == "measure") {
        m_measuredJson += text;
        // Extract the JSON block when it arrives.
        const int start = m_measuredJson.lastIndexOf('{');
        if (start >= 0) {
            // Try to parse the tail as JSON.
            const QString tail = m_measuredJson.mid(start);
            const auto doc = QJsonDocument::fromJson(tail.toUtf8());
            if (!doc.isNull() && doc.object().contains("input_i"))
                m_measuredJson = tail;
        }
    } else if (m_stage == "encode") {
        // Total duration comes from ffmpeg's own header, so no blocking
        // ffprobe run is needed on the UI thread.
        if (m_duration <= 0) {
            static const QRegularExpression reDur(R"re(Duration: (\d+):(\d+):(\d+)\.(\d+))re");
            const auto d = reDur.match(text);
            if (d.hasMatch())
                m_duration = d.captured(1).toDouble() * 3600 + d.captured(2).toDouble() * 60
                    + d.captured(3).toDouble() + d.captured(4).toDouble() / 100.0;
        }
        // Progress: time=HH:MM:SS.ss / duration.
        static const QRegularExpression reTime(R"re(time=(\d+):(\d+):(\d+)\.(\d+))re");
        const auto m = reTime.match(text);
        if (m.hasMatch() && m_duration > 0 && m_currentIndex >= 0 && m_currentIndex < m_items.size()) {
            const double sec = m.captured(1).toDouble() * 3600 + m.captured(2).toDouble() * 60
                + m.captured(3).toDouble() + m.captured(4).toDouble() / 100.0;
            const int pct = qMax(5, qMin(99, int((sec / m_duration) * 100)));
            VideoItem &it = m_items[m_currentIndex];
            if (pct != it.progress) {
                it.progress = pct;
                emit videosChanged();
            }
        }
    }
}

void Backend::onProcessFinished(int code, QProcess::ExitStatus status)
{
    if (m_stage == "crop") {
        if (code == 0) {
            // cropdetect reports a full-frame crop (e.g. 1920:1080:0:0) on
            // videos with no bars. Drop it so video can be copied, not
            // re-encoded.
            if (!m_cropString.isEmpty() && m_probeW > 0 && m_probeH > 0
                && m_cropW >= m_probeW && m_cropH >= m_probeH)
                m_cropString.clear();
            appendLog(tr("Black bars: %1").arg(m_cropString.isEmpty() ? tr("none") : m_cropString));
        } else {
            // Treat analysis failure as "no crop" and continue.
            appendLog(tr("Crop analysis failed; continuing without crop."));
            m_cropString.clear();
        }
        m_probeW = m_probeH = 0;
        startLoudnormMeasure();
        return;
    }
    if (m_stage == "measure") {
        if (code != 0) {
            const QString why = lastStderrLine();
            completeCurrent(false, why.isEmpty()
                ? tr("Loudness measurement failed (code %1).").arg(code)
                : tr("Loudness measurement failed: %1").arg(why));
            return;
        }
        // m_measuredJson already holds the JSON tail (or latest chunk).
        startFinalEncode();
        return;
    }
    if (m_stage == "encode") {
        if (code == 0 && status == QProcess::NormalExit && QFileInfo::exists(m_tmpOutput)) {
            // Atomic publish: rename into place. uniqueOutputPath picked an
            // unused name, so rename is safe.
            QFile::remove(m_finalOutput); // in case of a race, still unused
            if (QFile::rename(m_tmpOutput, m_finalOutput)) {
                VideoItem &it = m_items[m_currentIndex];
                it.outputPath = m_finalOutput;
                it.progress = 100;
                it.status = "Done";
                appendLog(tr("Wrote %1").arg(m_finalOutput));
                if (m_removeOriginal && QFile::remove(it.path)) {
                    appendLog(tr("Removed original %1").arg(it.path));
                }
                completeCurrent(true, tr("Done."));
            } else {
                completeCurrent(false, tr("Could not move the finished file into place."));
            }
        } else {
            const QString why = lastStderrLine();
            completeCurrent(false, why.isEmpty()
                ? tr("Processing failed (code %1).").arg(code)
                : tr("Processing failed: %1").arg(why));
        }
        return;
    }
    // probe or other stage: nothing to do.
}

void Backend::completeCurrent(bool ok, const QString &message)
{
    if (m_currentIndex >= 0 && m_currentIndex < m_items.size()) {
        VideoItem &it = m_items[m_currentIndex];
        it.status = ok ? "Done" : QString("Failed: %1").arg(message);
        if (!ok)
            it.progress = 0;
    }
    setStatus(message);
    appendLog(message);
    emit videosChanged();
    m_currentIndex = -1;
    m_cropString.clear();
    m_measuredJson.clear();
    if (!m_queue.isEmpty())
        startNextInQueue();
    else
        setBusy(false);
}

void Backend::onDirectoryChanged(const QString &path)
{
    (void)path;
    m_debounce.start();
}

void Backend::onDebounceTimeout()
{
    // Never swap the file list out from under a running pipeline — a rescan
    // mid-processing made indexes point at different files. Wait for idle.
    if (m_busy) {
        m_debounce.start();
        return;
    }
    collectFiles();
    if (m_autoProcess) {
        QList<int> fresh;
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items[i].status == "Pending")
                fresh.append(i);
        if (!fresh.isEmpty()) {
            setBusy(true);
            m_queue = fresh;
            startNextInQueue();
        }
    }
}

void Backend::clearLog()
{
    m_log.clear();
    setStatus("");
}

void Backend::openDestination() const
{
    if (!m_destDir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_destDir));
}
