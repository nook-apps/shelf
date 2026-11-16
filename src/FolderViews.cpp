#include "FolderViews.h"

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
    const QFileInfo info(path);
    return info.isDir() ? info.canonicalFilePath() : QString();
}
QJsonObject readViews()
{
    QFile f(fileName());
    if (!f.open(QIODevice::ReadOnly)) return {};
    auto entries = QJsonDocument::fromJson(f.readAll()).object();
    for (const auto &path : entries.keys())
        if (key(path).isEmpty()) entries.remove(path);
    return entries;
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

QVariantMap FolderViews::get(const QString &path) const
{
    const auto canonical = key(path);
    if (canonical.isEmpty()) return {};
    return validated(readViews().value(canonical).toObject().toVariantMap());
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
    // Несколько окон не должны затирать изменения друг друга.
    QLockFile lock(fileName() + QStringLiteral(".lock"));
    if (!lock.tryLock(100)) return false;
    auto entries = readViews();
    if (view.isEmpty()) entries.remove(canonical);
    else {
        auto entry = QJsonObject::fromVariantMap(view);
        // Даже несколько правок за одну миллисекунду имеют однозначный порядок.
        qint64 saved = QDateTime::currentMSecsSinceEpoch();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const auto previous = it.value().toObject().value(QStringLiteral("saved")).toInteger();
            if (previous >= saved && previous < std::numeric_limits<qint64>::max()) saved = previous + 1;
        }
        entry.insert(QStringLiteral("saved"), saved);
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
    if (!f.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(entries).toJson(QJsonDocument::Compact);
    return f.write(bytes) == bytes.size() && f.commit();
}
