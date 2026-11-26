#include "FolderViews.h"
#include "RemotePath.h"

#include <limits>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
QString fileName()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/shelf/folder-views.json");
}
QString key(const QString &path)
{
    if (isRemotePath(path)) return {};
    const QFileInfo info(path);
    return info.isDir() ? info.canonicalFilePath() : QString();
}
QJsonObject readViews()
{
    QFile f(fileName());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
QVariantMap validated(const QVariantMap &view)
{
    const auto sort = view.value(QStringLiteral("sortKey")).toString();
    const int size = view.value(QStringLiteral("iconSize")).toInt();
    if (!QStringList{QStringLiteral("name"), QStringLiteral("date"), QStringLiteral("size"), QStringLiteral("type"), QStringLiteral("ext"), QStringLiteral("created")}.contains(sort)
        || !QList<int>{40, 56, 80, 112, 160}.contains(size)
        || !view.contains(QStringLiteral("gridMode")) || !view.contains(QStringLiteral("sortDesc"))) return {};
    const auto group = view.value(QStringLiteral("groupBy")).toString();
    if (!QStringList{"", "type", "date"}.contains(group)) return {};
    return {{QStringLiteral("groupBy"), group}, {QStringLiteral("sortKey"), sort}, {QStringLiteral("sortDesc"), view.value(QStringLiteral("sortDesc")).toBool()},
            {QStringLiteral("gridMode"), view.value(QStringLiteral("gridMode")).toBool()}, {QStringLiteral("iconSize"), size}};
}
}

FolderViews::FolderViews(QObject *parent) : QObject(parent)
{
    m_writeTimer.setSingleShot(true);
    m_writeTimer.setInterval(250);
    connect(&m_writeTimer, &QTimer::timeout, this, [this] { flush(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { reload(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { reload(); });
}

FolderViews::~FolderViews() { flush(100); }

void FolderViews::reload()
{
    const QString file = fileName();
    const QString parent = QFileInfo(file).absolutePath();
    // Watch an existing ancestor too, so a first save in another process is detected.
    QString ancestor = parent;
    while (!QFileInfo(ancestor).isDir() && ancestor != QLatin1String("/"))
        ancestor = QFileInfo(ancestor).absolutePath();
    if (!m_watcher.directories().contains(ancestor)) {
        const auto previous = m_watcher.directories();
        if (!previous.isEmpty()) m_watcher.removePaths(previous);
        m_watcher.addPath(ancestor);
    }
    if (QFileInfo::exists(file) && !m_watcher.files().contains(file))
        m_watcher.addPath(file);
    m_entries = readViews();
    m_loaded = true;
}

QVariantMap FolderViews::get(const QString &path)
{
    if (!m_loaded) reload();
    const auto canonical = key(path);
    if (canonical.isEmpty()) return {};
    const auto pending = m_changes.constFind(canonical);
    return pending != m_changes.cend() ? *pending : validated(m_entries.value(canonical).toObject().toVariantMap());
}
bool FolderViews::save(const QString &path, const QVariantMap &view)
{
    const auto clean = validated(view);
    return !clean.isEmpty() && update(path, clean);
}
bool FolderViews::reset(const QString &path) { return update(path, {}); }
bool FolderViews::update(const QString &path, const QVariantMap &view)
{
    const auto canonical = key(path);
    if (canonical.isEmpty() || !QDir().mkpath(QFileInfo(fileName()).absolutePath())) return false;
    m_changes.insert(canonical, view);
    m_order.removeAll(canonical);
    m_order.append(canonical);
    if (!m_writeTimer.isActive()) m_writeTimer.start();
    return true;
}

bool FolderViews::flush(int lockWait)
{
    if (m_changes.isEmpty()) return true;
    // Несколько окон не должны затирать изменения друг друга.
    QLockFile lock(fileName() + QStringLiteral(".lock"));
    if (!lock.tryLock(lockWait)) {
        if (!lockWait && lock.error() == QLockFile::LockFailedError) m_writeTimer.start();
        else emit writeFailed();
        return false;
    }
    auto entries = readViews();
    if (entries.size() + m_changes.size() > limit) {
        for (const auto &path : entries.keys())
            if (key(path).isEmpty()) entries.remove(path);
    }
    qint64 saved = QDateTime::currentMSecsSinceEpoch();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const auto previous = it.value().toObject().value(QStringLiteral("saved")).toInteger();
        if (previous >= saved && previous < std::numeric_limits<qint64>::max()) saved = previous + 1;
    }
    for (const QString &canonical : std::as_const(m_order)) {
        const auto &view = m_changes[canonical];
        if (view.isEmpty()) { entries.remove(canonical); continue; }
        auto entry = QJsonObject::fromVariantMap(view);
        entry.insert(QStringLiteral("saved"), saved);
        if (saved < std::numeric_limits<qint64>::max()) ++saved;
        entries.insert(canonical, entry);
    }
    while (entries.size() > limit) {
        QString oldest;
        qint64 time = std::numeric_limits<qint64>::max();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const auto saved = it.value().toObject().value(QStringLiteral("saved")).toInteger();
            if (oldest.isEmpty() || saved < time) { oldest = it.key(); time = saved; }
        }
        entries.remove(oldest);
    }
    QSaveFile f(fileName());
    const auto bytes = QJsonDocument(entries).toJson(QJsonDocument::Compact);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        emit writeFailed();
        return false;
    }
    m_changes.clear();
    m_order.clear();
    reload();
    return true;
}
