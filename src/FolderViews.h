#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// Только явно изменённый вид; чтение папки не создаёт запись.
class FolderViews : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit FolderViews(QObject *parent = nullptr);
    ~FolderViews() override;
    Q_INVOKABLE QVariantMap get(const QString &path);
    Q_INVOKABLE bool save(const QString &path, const QVariantMap &view);
    Q_INVOKABLE bool reset(const QString &path);
    static constexpr int limit = 512;
signals:
    void writeFailed();
private:
    friend class TestCore;
    bool update(const QString &path, const QVariantMap &view);
    void reload();
    bool flush(int lockWait = 0);
    QJsonObject m_entries;
    QMap<QString, QVariantMap> m_changes;
    QStringList m_order;
    QFileSystemWatcher m_watcher;
    QTimer m_writeTimer;
    bool m_loaded = false;
};
