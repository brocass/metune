// Portal file picker follows Omacut (MIT, David Heinemeier Hansson).
// Uses xdg-desktop-portal FileChooser over D-Bus instead of QFileDialog,
// so the dialog matches the Wayland compositor session.
#include "portalfilepicker.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusInterface>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <unistd.h>

void PortalFilePicker::pickDirectory(const QString &initialDirectory)
{
    if (m_waiting) {
        emit failed(tr("A folder picker is already open."));
        return;
    }

    const QString parent = initialDirectory.isEmpty() ? QDir::homePath() : initialDirectory;
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestPathString = "/org/freedesktop/portal/desktop/request/"
        + QString("1_%1/%2").arg(QString::number(getpid())).arg(token);

    // Ask the portal for an OpenFile handle, then watch its Response signal.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(tr("No D-Bus session; cannot open the folder picker."));
        return;
    }

    QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.FileChooser", bus);
    if (!portal.isValid()) {
        emit failed(tr("The file chooser portal is not available."));
        return;
    }

    m_requestPath = QDBusObjectPath(requestPathString);
    bus.connect("org.freedesktop.portal.Desktop", requestPathString,
                "org.freedesktop.portal.Request", "Response",
                this, SLOT(onResponse(uint, QVariantMap)));

    QVariantMap options;
    options.insert("handle", QVariant::fromValue(m_requestPath));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    options.insert("directory", true);
    options.insert("multiple", false);
#else
    options.insert("directory", true);
#endif
    if (QFileInfo::exists(parent))
        options.insert("current_folder", parent);

    QDBusMessage msg = portal.call("OpenFile", "", QVariantMap(), options);
    // The reply handle is also delivered via Response; if the call itself
    // errors, report it.
    if (msg.type() == QDBusMessage::ErrorMessage) {
        m_waiting = false;
        emit failed(tr("Could not open the folder picker: %1").arg(msg.errorMessage()));
        return;
    }
    m_waiting = true;
}

void PortalFilePicker::onResponse(uint response, const QVariantMap &results)
{
    m_waiting = false;
    if (response != 0) {
        emit failed(tr("Folder selection was cancelled."));
        return;
    }
    const QVariantList uris = results.value("uris").value<QVariantList>();
    if (uris.isEmpty()) {
        emit failed(tr("No folder was selected."));
        return;
    }
    const QString uri = uris.first().toString();
    // file:///path -> /path
    QString path = uri;
    if (path.startsWith("file://"))
        path = path.mid(7);
    emit picked(path);
}
