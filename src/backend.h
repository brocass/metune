#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFileSystemWatcher>
#include <QStringList>
#include <QVariant>

struct ParsedName {
    QString artist;
    QString title;
};

struct VideoItem {
    QString path;
    QString fileName;
    QString artist;
    QString title;
    QString year;
    QString genre;
    QString status = "Pending";
    int progress = 0;
    QString outputPath;
    double duration = 0;
};

class FilePicker;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString watchDir READ watchDir NOTIFY settingsChanged)
    Q_PROPERTY(QString destDir READ destDir NOTIFY settingsChanged)
    Q_PROPERTY(bool normalize READ normalize NOTIFY settingsChanged)
    Q_PROPERTY(bool cropBars READ cropBars NOTIFY settingsChanged)
    Q_PROPERTY(bool autoProcess READ autoProcess NOTIFY settingsChanged)
    Q_PROPERTY(bool removeOriginal READ removeOriginal NOTIFY settingsChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList videos READ videos NOTIFY videosChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QString watchDir() const { return m_watchDir; }
    QString destDir() const { return m_destDir; }
    bool normalize() const { return m_normalize; }
    bool cropBars() const { return m_cropBars; }
    bool autoProcess() const { return m_autoProcess; }
    bool removeOriginal() const { return m_removeOriginal; }
    int selectedIndex() const { return m_selectedIndex; }
    QVariantList videos() const;
    QString statusText() const { return m_statusText; }
    bool busy() const { return m_busy; }

    Q_INVOKABLE void setWatchDir(const QString &path);
    Q_INVOKABLE void setDestDir(const QString &path);
    Q_INVOKABLE void setNormalize(bool v);
    Q_INVOKABLE void setCropBars(bool v);
    Q_INVOKABLE void setAutoProcess(bool v);
    Q_INVOKABLE void setRemoveOriginal(bool v);

    Q_INVOKABLE void pickWatchDir();
    Q_INVOKABLE void pickDestDir();
    Q_INVOKABLE void scan();
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void updateField(int index, const QString &field, const QString &value);
    Q_INVOKABLE void lookupSelected();
    Q_INVOKABLE void lookup(int index);
    Q_INVOKABLE void process(int index);
    Q_INVOKABLE void processAll();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clearLog();
    Q_INVOKABLE void openDestination() const;

    Q_INVOKABLE QString parsedArtist(const QString &fileName) const;
    Q_INVOKABLE QString parsedTitle(const QString &fileName) const;

    static ParsedName parseFilename(const QString &fileName);
    static QString cleanTitle(const QString &title);
    static QString cleanedFileName(const QString &artist, const QString &title, const QString &ext);
    static QString decadeForYear(int year);
    static bool isExcludedPath(const QString &path);
    static QStringList supportedExtensions();
    static int yearFromDateString(const QString &date);

signals:
    void settingsChanged();
    void selectionChanged();
    void videosChanged();
    void statusChanged();
    void busyChanged();
    void logLine(const QString &text);
    void pickerFailed(const QString &reason);

private slots:
    void onPickerPicked(const QString &path);
    void onDirectoryChanged(const QString &path);
    void onDebounceTimeout();
    void onLookupFinished(QNetworkReply *reply);
    void onProcessReadyRead();
    void onProcessFinished(int code, QProcess::ExitStatus status);

private:
    void setStatus(const QString &s);
    void setBusy(bool b);
    void appendLog(const QString &s);
    void refreshVideoAt(int index);
    void startNextInQueue();
    void startLookupForQueue();
    void startCropDetect();
    void startLoudnormMeasure();
    void startFinalEncode();
    void completeCurrent(bool ok, const QString &message);
    void rearmWatcher();
    void loadSettings();
    void saveSettings();
    void collectFiles();
    int findIndexByPath(const QString &path) const;
    QString uniqueOutputPath(const QString &cleanName) const;
    QString lastStderrLine() const;

    QString m_watchDir;
    QString m_destDir;
    bool m_normalize = true;
    bool m_cropBars = true;
    bool m_autoProcess = false;
    bool m_removeOriginal = false;

    int m_selectedIndex = -1;
    QList<VideoItem> m_items;
    QString m_statusText;
    bool m_busy = false;

    FilePicker *m_picker = nullptr;
    bool m_pickingWatch = true;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;

    QNetworkAccessManager m_net;
    QNetworkReply *m_lookupReply = nullptr;
    int m_lookupIndex = -1;

    QProcess m_process;
    QString m_stage; // "await-lookup", "crop", "measure", "encode"
    int m_currentIndex = -1;
    QList<int> m_queue;
    QString m_cropString;
    QString m_measuredJson;
    QString m_tmpOutput;
    QString m_finalOutput;
    double m_duration = 0;
    QString m_stderrTail;
    int m_probeW = 0, m_probeH = 0;
    int m_cropW = 0, m_cropH = 0;

    QStringList m_log;
};
