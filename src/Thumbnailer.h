#pragma once

#include <QCache>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class Thumbnailer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit Thumbnailer(QObject *parent = nullptr);
    int revision() const { return m_revision; }

    Q_INVOKABLE QString thumbnail(const QString &path, const QString &mimeType, int size = 56, qint64 mtime = -1);
    Q_INVOKABLE void watch(const QString &path);
    Q_INVOKABLE void unwatch(const QString &path);
    static QString cacheName(const QString &path);

signals:
    void revisionChanged();
    void pathChanged(const QString &path);

private slots:
    void onReady(uint handle, const QStringList &uris);
    void onError(uint handle, const QStringList &uris, int code, const QString &message);
    void onFinished(uint handle);

private:
    friend class TestResources;
    struct Known { qint64 mtime; QString url; };
    struct Lookup {
        QString path, mime;
        int flavor;
        qint64 mtime;
        QString url;
        bool complete = false, failed = false;
    };

    void loadSupported();
    int flavorFor(int size) const;
    void lookupQueued();
    void flush();
    void dequeueUnused();
    void serviceChanged(const QString &oldOwner, const QString &newOwner);
    void notifyPath(const QString &path);

    QString m_cacheDir;
    QSet<QString> m_supported;
    int m_maxFlavor = 1;
    QCache<QString, Known> m_known{4096};
    QCache<QString, qint64> m_never{4096};
    QCache<QString, qint64> m_unavailable{4096};
    QHash<QString, int> m_watchers;
    QHash<QString, Lookup> m_checkQueued;
    QHash<QString, qint64> m_checkRunning;
    QFutureWatcher<QList<Lookup>> m_checker;
    QHash<int, QHash<QString, Lookup>> m_queue;
    QHash<QString, qint64> m_pending;
    QHash<uint, QHash<QString, QString>> m_handles;
    QTimer m_lookupTimer;
    QTimer m_flushTimer;
    int m_generation = 0;
    int m_revision = 0;
};
