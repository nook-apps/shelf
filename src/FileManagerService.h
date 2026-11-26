#pragma once

#include <QDBusAbstractAdaptor>
#include <QStringList>
#include <QVariantMap>

class FileManagerService : public QObject
{
    Q_OBJECT

public:
    explicit FileManagerService(QObject *parent = nullptr);

public slots:
    void openWindow(const QString &path) { emit openRequested({path}, QStringLiteral("window")); }
    void quit() { emit quitRequested(); }

signals:
    void openRequested(const QStringList &paths, const QString &action);
    void quitRequested();
};

class FileManager1 : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")

public:
    explicit FileManager1(FileManagerService *service) : QDBusAbstractAdaptor(service), m_service(service) {}

public slots:
    void ShowItems(const QStringList &uris, const QString &) { emit m_service->openRequested(uris, QStringLiteral("select")); }
    void ShowFolders(const QStringList &uris, const QString &) { emit m_service->openRequested(uris, QStringLiteral("open")); }
    void ShowItemProperties(const QStringList &uris, const QString &) { emit m_service->openRequested(uris, QStringLiteral("properties")); }

private:
    FileManagerService *m_service;
};

class DesktopApplication : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public:
    explicit DesktopApplication(FileManagerService *service) : QDBusAbstractAdaptor(service), m_service(service) {}

public slots:
    void Open(const QStringList &uris, const QVariantMap &platform) {
        emit m_service->openRequested(uris, platform.value(QStringLiteral("select")).toBool()
            ? QStringLiteral("select") : QStringLiteral("open"));
    }
    void Activate(const QVariantMap &) { emit m_service->openRequested({}, QStringLiteral("activate")); }
    void ActivateAction(const QString &action, const QVariantList &, const QVariantMap &) {
        if (action == QLatin1String("quit")) m_service->quit();
    }

private:
    FileManagerService *m_service;
};
