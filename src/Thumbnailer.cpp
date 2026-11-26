#include "Thumbnailer.h"
#include "RemotePath.h"

#include <QDBusServiceWatcher>
#include <QtConcurrent>
#include <algorithm>

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QtEndian>
#include <QStandardPaths>
#include <QUrl>

namespace {
const QString kService = QStringLiteral("org.freedesktop.thumbnails.Thumbnailer1");
const QString kPath = QStringLiteral("/org/freedesktop/thumbnails/Thumbnailer1");
const QString kIface = QStringLiteral("org.freedesktop.thumbnails.Thumbnailer1");
// Размеры миниатюр по спецификации: сторона 128, 256, 512 и 1024 px
const char *const kFlavors[] = {"normal", "large", "x-large", "xx-large"};
constexpr int kFlavorPx[] = {128, 256, 512, 1024};

QString pendingKey(int flavor, const QString &path)
{
    return QString::number(flavor) + QLatin1Char(':') + path;
}

qint64 mtimeOf(const QString &path)
{
    return QFileInfo(path).lastModified().toSecsSinceEpoch();
}

// Поле tEXt из PNG. QImageReader::text() до чтения картинки режет ключ по первому
// двоеточию («Thumb::MTime» → «Thumb»), а читать всю картинку ради поля дорого.
// Текстовые чанки миниатюр лежат до IDAT, так что хватает начала файла.
QByteArray pngText(const QString &file, const QByteArray &key)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = f.read(8192);
    if (!head.startsWith("\x89PNG\r\n\x1a\n"))
        return {};
    qsizetype pos = 8;
    while (pos + 8 <= head.size()) {
        const quint32 len = qFromBigEndian<quint32>(head.constData() + pos);
        const QByteArray type = head.mid(pos + 4, 4);
        if (type == "IDAT" || type == "IEND" || pos + 12 + qsizetype(len) > head.size())
            break;
        if (type == "tEXt") {
            const QByteArray data = head.mid(pos + 8, len);
            const qsizetype zero = data.indexOf('\0');
            if (zero > 0 && data.left(zero) == key)
                return data.mid(zero + 1);
        }
        pos += 12 + len;
    }
    return {};
}

// По спецификации MTime — целые секунды, но новые tumbler и GLib пишут
// с долями («1792680195.046882»), поэтому сравниваем целую часть
bool sameMtime(const QString &thumb, qint64 mtime)
{
    bool ok = false;
    const double t = pngText(thumb, "Thumb::MTime").toDouble(&ok);
    return ok && qint64(t) == mtime;
}
}

Thumbnailer::Thumbnailer(QObject *parent)
    : QObject(parent)
    , m_cacheDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                 + QStringLiteral("/thumbnails"))
{
    m_lookupTimer.setSingleShot(true);
    m_lookupTimer.setInterval(15);
    connect(&m_lookupTimer, &QTimer::timeout, this, &Thumbnailer::lookupQueued);
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(40);
    connect(&m_flushTimer, &QTimer::timeout, this, &Thumbnailer::flush);
    connect(&m_checker, &QFutureWatcher<QList<Lookup>>::finished, this, [this] {
        const auto results = m_checker.result();
        m_checkRunning.clear();
        for (const auto &result : results) {
            const auto key = pendingKey(result.flavor, result.path);
            if (m_checkQueued.contains(key) && m_checkQueued.value(key).mtime != result.mtime) continue;
            if (result.failed) {
                m_never.insert(result.path, new qint64(result.mtime));
            } else if (result.complete) {
                m_known.insert(key, new Known{result.mtime, result.url});
            } else {
                m_known.insert(key, new Known{result.mtime, result.url});
                m_unavailable.insert(key, new qint64(result.mtime));
                if (m_supported.contains(result.mime) && m_watchers.contains(result.path) && !m_pending.contains(key)) {
                    m_queue[result.flavor].insert(result.path, result);
                    if (!m_flushTimer.isActive()) m_flushTimer.start();
                }
            }
            notifyPath(result.path);
        }
        if (!m_checkQueued.isEmpty()) m_lookupTimer.start();
    });

    auto bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kIface, QStringLiteral("Ready"), this, SLOT(onReady(uint, QStringList)));
    bus.connect(kService, kPath, kIface, QStringLiteral("Error"), this, SLOT(onError(uint, QStringList, int, QString)));
    bus.connect(kService, kPath, kIface, QStringLiteral("Finished"), this, SLOT(onFinished(uint)));
    auto *watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &oldOwner, const QString &newOwner) {
        serviceChanged(oldOwner, newOwner);
    });
    QTimer::singleShot(0, this, &Thumbnailer::loadSupported);
}

int Thumbnailer::flavorFor(int size) const
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    const qreal px = size * (screen ? screen->devicePixelRatio() : 1.0);
    int f = 0;
    while (f < m_maxFlavor && kFlavorPx[f] < px) ++f;
    return f;
}

QString Thumbnailer::cacheName(const QString &path)
{
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    return QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex()) + QStringLiteral(".png");
}

void Thumbnailer::loadSupported()
{
    const int generation = m_generation;
    auto bus = QDBusConnection::sessionBus();
    auto call = [this, bus](const QString &method) {
        return new QDBusPendingCallWatcher(bus.asyncCall(QDBusMessage::createMethodCall(kService, kPath, kIface, method)), this);
    };
    connect(call(QStringLiteral("GetSupported")), &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<QStringList, QStringList> reply = *w;
        w->deleteLater();
        if (generation != m_generation) return;
        if (!reply.isError()) {
            const auto schemes = reply.argumentAt<0>(), mimes = reply.argumentAt<1>();
            for (qsizetype i = 0; i < qMin(schemes.size(), mimes.size()); ++i)
                if (schemes[i] == QLatin1String("file")) m_supported.insert(mimes[i]);
            for (const auto &key : m_unavailable.keys()) m_known.remove(key);
            m_unavailable.clear();
            ++m_revision;
            emit revisionChanged();
        }
    });
    connect(call(QStringLiteral("GetFlavors")), &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<QStringList> reply = *w;
        w->deleteLater();
        if (generation != m_generation || reply.isError()) return;
        const int old = m_maxFlavor;
        const QStringList flavors = reply.value();
        for (int i = 0; i < 4; ++i)
            if (flavors.contains(QLatin1String(kFlavors[i]))) m_maxFlavor = qMax(m_maxFlavor, i);
        if (old != m_maxFlavor) { ++m_revision; emit revisionChanged(); }
    });
}

void Thumbnailer::serviceChanged(const QString &oldOwner, const QString &newOwner)
{
    if (oldOwner.isEmpty() && newOwner.isEmpty()) return;
    ++m_generation;
    m_handles.clear();
    m_pending.clear();
    m_queue.clear();
    m_supported.clear();
    for (const auto &key : m_unavailable.keys()) m_known.remove(key);
    m_unavailable.clear();
    m_never.clear();
    m_maxFlavor = 1;
    ++m_revision;
    emit revisionChanged();
    if (!newOwner.isEmpty()) loadSupported();
}

QString Thumbnailer::thumbnail(const QString &path, const QString &mimeType, int size, qint64 mtime)
{
    if (path.isEmpty() || path.startsWith(m_cacheDir + QLatin1Char('/')) || isRemotePath(path)) return {};
    const auto *failed = m_never.object(path);
    if (failed && *failed == mtime) return {};
    const int flavor = flavorFor(size);
    const QString key = pendingKey(flavor, path);
    const auto *cached = m_known.object(key);
    const auto *unavailable = m_unavailable.object(key);
    if (cached && cached->mtime == mtime && (!unavailable || *unavailable != mtime || !m_supported.contains(mimeType)
        || m_pending.contains(key) || m_queue.value(flavor).contains(path))) return cached->url;
    if (m_checkRunning.value(key, -2) != mtime &&
        (!m_checkQueued.contains(key) || m_checkQueued.value(key).mtime != mtime)) {
        m_checkQueued.insert(key, {path, mimeType, flavor, mtime, {}, false, false});
        if (!m_checker.isRunning() && !m_lookupTimer.isActive()) m_lookupTimer.start();
    }
    return cached && cached->mtime == mtime ? cached->url : QString();
}

void Thumbnailer::watch(const QString &path)
{
    if (!path.isEmpty()) ++m_watchers[path];
}

void Thumbnailer::unwatch(const QString &path)
{
    auto found = m_watchers.find(path);
    if (found == m_watchers.end() || --*found > 0) return;
    m_watchers.erase(found);
    for (int flavor = 0; flavor < 4; ++flavor) {
        m_checkQueued.remove(pendingKey(flavor, path));
        auto queue = m_queue.find(flavor);
        if (queue != m_queue.end()) queue->remove(path);
    }
    dequeueUnused();
}

void Thumbnailer::lookupQueued()
{
    if (m_checker.isRunning() || m_checkQueued.isEmpty()) return;
    auto items = std::exchange(m_checkQueued, {}).values();
    for (const auto &item : std::as_const(items)) m_checkRunning.insert(pendingKey(item.flavor, item.path), item.mtime);
    m_checker.setFuture(QtConcurrent::run([items = std::move(items), root = m_cacheDir]() mutable {
        const QDir fail(root + QStringLiteral("/fail"));
        const auto failedBy = fail.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (auto &item : items) {
            const qint64 mtime = item.mtime >= 0 ? item.mtime : mtimeOf(item.path);
            const auto name = cacheName(item.path);
            auto read = [&](int size) -> QString {
                const QString file = root + QLatin1Char('/') + QLatin1String(kFlavors[size]) + QLatin1Char('/') + name;
                if (!sameMtime(file, mtime)) return {};
                auto url = QUrl::fromLocalFile(file);
                url.setQuery(QString::number(mtime));
                return url.toString();
            };
            for (int size = item.flavor; size < 4 && item.url.isEmpty(); ++size) item.url = read(size);
            item.complete = !item.url.isEmpty();
            if (item.complete) continue;
            for (const auto &app : failedBy) {
                if (sameMtime(fail.filePath(app + QLatin1Char('/') + name), mtime)) { item.failed = true; break; }
            }
            if (!item.failed)
                for (int size = item.flavor - 1; size >= 0 && item.url.isEmpty(); --size) item.url = read(size);
        }
        return items;
    }));
}

void Thumbnailer::flush()
{
    const int generation = m_generation;
    auto bus = QDBusConnection::sessionBus();
    const auto queues = std::exchange(m_queue, {});
    for (auto q = queues.cbegin(); q != queues.cend(); ++q) {
        QStringList uris, mimes;
        QHash<QString, QString> keys;
        for (const auto &lookup : *q) {
            if (!m_watchers.contains(lookup.path)) continue;
            const QString uri = QUrl::fromLocalFile(lookup.path).toString(QUrl::FullyEncoded);
            const QString key = pendingKey(q.key(), lookup.path);
            uris << uri;
            mimes << lookup.mime;
            keys.insert(uri, key);
            m_pending.insert(key, lookup.mtime);
        }
        if (uris.isEmpty()) continue;
        auto msg = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("Queue"));
        msg << uris << mimes << QString::fromLatin1(kFlavors[q.key()]) << QStringLiteral("foreground") << uint(0);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, keys, generation](QDBusPendingCallWatcher *w) {
            const QDBusPendingReply<uint> reply = *w;
            w->deleteLater();
            if (generation != m_generation) return;
            if (!reply.isError()) {
                m_handles.insert(reply.value(), keys);
                dequeueUnused();
            } else {
                for (auto it = keys.cbegin(); it != keys.cend(); ++it)
                    m_never.insert(QUrl(it.key()).toLocalFile(), new qint64(m_pending.take(it.value())));
            }
        });
    }
}

void Thumbnailer::dequeueUnused()
{
    for (auto it = m_handles.begin(); it != m_handles.end();) {
        const auto uris = it->keys();
        const bool wanted = std::any_of(uris.cbegin(), uris.cend(), [this](const auto &uri) {
            return m_watchers.contains(QUrl(uri).toLocalFile());
        });
        if (wanted) { ++it; continue; }
        auto msg = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("Dequeue"));
        msg << it.key();
        QDBusConnection::sessionBus().asyncCall(msg);
        for (const auto &key : *it) m_pending.remove(key);
        it = m_handles.erase(it);
    }
}

void Thumbnailer::notifyPath(const QString &path)
{
    if (m_watchers.contains(path)) emit pathChanged(path);
}

void Thumbnailer::onReady(uint handle, const QStringList &uris)
{
    const auto keys = m_handles.value(handle);
    for (const auto &uri : uris) {
        const QString path = QUrl(uri).toLocalFile();
        const auto key = keys.value(uri);
        if (key.isEmpty()) continue;
        m_pending.remove(key);
        m_known.remove(key);
        m_unavailable.remove(key);
        notifyPath(path);
    }
}

void Thumbnailer::onError(uint handle, const QStringList &uris, int, const QString &)
{
    const auto keys = m_handles.value(handle);
    for (const auto &uri : uris) {
        const auto key = keys.value(uri);
        if (key.isEmpty() || !m_pending.contains(key)) continue;
        const auto path = QUrl(uri).toLocalFile();
        m_never.insert(path, new qint64(m_pending.take(key)));
        notifyPath(path);
    }
}

void Thumbnailer::onFinished(uint handle)
{
    const auto keys = m_handles.take(handle);
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        if (!m_pending.contains(it.value())) continue;
        m_never.insert(QUrl(it.key()).toLocalFile(), new qint64(m_pending.take(it.value())));
    }
}
