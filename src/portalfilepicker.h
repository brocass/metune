#pragma once
// Portal file picker follows Omacut (MIT, David Heinemeier Hansson).
#include "filepicker.h"

#include <QDBusObjectPath>

class PortalFilePicker : public FilePicker {
    Q_OBJECT
public:
    explicit PortalFilePicker(QObject *parent = nullptr) : FilePicker(parent) {}
    void pickDirectory(const QString &initialDirectory = {}) override;

private slots:
    void onResponse(uint response, const QVariantMap &results);

private:
    void tryOpenFile(const QString &initialDirectory, const QDBusObjectPath &requestPath);

    QDBusObjectPath m_requestPath;
    bool m_waiting = false;
};
