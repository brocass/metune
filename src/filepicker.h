#pragma once
#include <QObject>
#include <QString>

// Abstract file picker; the portal implementation in portalfilepicker.*
// is the production default, and tests inject a fake by subclassing.
class FilePicker : public QObject {
    Q_OBJECT
public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual void pickDirectory(const QString &initialDirectory = {}) = 0;

signals:
    void picked(const QString &path);
    void failed(const QString &reason);
};
