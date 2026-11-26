#include "DirModel.h"

#include "Language.h"
#include "RemotePath.h"
#include "Trash.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QQueue>
#include <QStorageInfo>
#include <QtConcurrent>

#include <algorithm>
#include <numeric>
#include <sys/inotify.h>
#include <unistd.h>

namespace {
// Как в quickshell/scripts/spotlight_search.py: в этих папках искать бессмысленно
bool skipInSearch(const QString &name, const QString &path)
{
    static const QStringList names = {QStringLiteral("node_modules"), QStringLiteral("__pycache__"),
                                      QStringLiteral(".git")};
    static const QStringList paths = {QStringLiteral("/proc"), QStringLiteral("/sys"), QStringLiteral("/dev"),
                                      QStringLiteral("/run")};
    if (names.contains(name) || paths.contains(path))
        return true;
    return path.endsWith(QLatin1String("/target/debug")) || path.endsWith(QLatin1String("/target/release"));
}

constexpr int kSearchLimit = 3000;

struct Ordering {
    QString key, group;
    bool descending, foldersFirst, trash;
    QCollator collator;

    bool less(const FileEntry &a, const FileEntry &b) const
    {
        if (!group.isEmpty() && a.group != b.group)
            return descending ? a.group > b.group : a.group < b.group;
        if (foldersFirst && a.isDir != b.isDir) return a.isDir;
        const auto compare = [](const auto &x, const auto &y) { return x < y ? -1 : y < x ? 1 : 0; };
        int c = 0;
        if (key == QLatin1String("size"))
            c = compare(a.isDir ? a.childCount : a.size, b.isDir ? b.childCount : b.size);
        else if (key == QLatin1String("ext"))
            c = QString::compare(a.extension, b.extension, Qt::CaseInsensitive);
        else if (key == QLatin1String("created"))
            c = compare(a.created, b.created);
        else if (key == QLatin1String("date"))
            c = compare(a.deleted.isValid() ? a.deleted : a.mtime, b.deleted.isValid() ? b.deleted : b.mtime);
        else if (key == QLatin1String("type"))
            c = a.typeKey && b.typeKey ? a.typeKey->compare(*b.typeKey)
                : collator.compare(trash ? a.origin : a.mimeComment, trash ? b.origin : b.mimeComment);
        if (!c) c = a.nameKey && b.nameKey ? a.nameKey->compare(*b.nameKey) : collator.compare(a.name, b.name);
        return descending ? c > 0 : c < 0;
    }

    QList<FileEntry> sorted(QList<FileEntry> rows) const
    {
        QList<qsizetype> order(rows.size());
        std::iota(order.begin(), order.end(), qsizetype(0));
        for (auto &e : rows) {
            if (!e.nameKey) e.nameKey.emplace(collator.sortKey(e.name));
            if (key == QLatin1String("type")) e.typeKey.emplace(collator.sortKey(trash ? e.origin : e.mimeComment));
        }
        std::stable_sort(order.begin(), order.end(), [&](qsizetype a, qsizetype b) { return less(rows.at(a), rows.at(b)); });
        QList<FileEntry> result;
        result.reserve(rows.size());
        for (const auto i : order) result.append(std::move(rows[i]));
        return result;
    }
};

template <typename T>
void stopTask(QFutureWatcher<T> *&task)
{
    if (!task) return;
    task->disconnect();
    task->cancel();
    task->deleteLater();
    task = nullptr;
}

QList<int> changedRoles(const FileEntry &old, const FileEntry &now)
{
    using R = DirModel;
    QList<int> roles;
    if (old.name != now.name) roles << R::NameRole;
    if (old.dir != now.dir) roles << R::DirRole;
    if (old.isDir != now.isDir) roles << R::IsDirRole;
    if (old.isHidden != now.isHidden) roles << R::IsHiddenRole;
    if (old.isLink != now.isLink) roles << R::IsLinkRole;
    if (old.isLocked != now.isLocked) roles << R::IsLockedRole;
    if (old.mtime != now.mtime) roles << R::MtimeRole;
    if (old.mtime != now.mtime || old.deleted != now.deleted) roles << R::DateTextRole;
    if (old.size != now.size) roles << R::SizeRole;
    if (old.size != now.size || old.childCount != now.childCount || old.isDir != now.isDir) roles << R::SizeTextRole;
    if (old.mimeType != now.mimeType) roles << R::MimeTypeRole;
    if (old.mimeComment != now.mimeComment) roles << R::MimeCommentRole;
    if (old.icon != now.icon) roles << R::IconRole;
    if (old.origin != now.origin) roles << R::OriginRole;
    if (old.group != now.group) roles << R::GroupIdRole << R::GroupLabelRole;
    return roles;
}
}

DirModel::DirModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    m_groupDate = QDate::currentDate();
    m_dateTimer.setInterval(60000);
    connect(&m_dateTimer, &QTimer::timeout, this, [this] {
        if (m_groupBy == QLatin1String("date") && m_groupDate != QDate::currentDate()) applyFilterAndSort();
    });
    m_dateTimer.start();
    m_countTimer.setSingleShot(true);
    m_countTimer.setInterval(30);
    connect(&m_countTimer, &QTimer::timeout, this, &DirModel::countFolders);

    // Копирование большой папки шлёт сотни событий — перечитываем не чаще раза в 200 мс
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(200);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        if (!m_query.isEmpty()) return;
        if (m_listingTask) { m_reloadTimer.start(200); return; }
        const auto changed = std::exchange(m_changedPaths, {});
        listDir(std::exchange(m_fullReload, false) ? QSet<QString>{} : changed);
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        m_fullReload = true;
        if (!m_reloadTimer.isActive()) m_reloadTimer.start(200);
    });
    // Другой язык — другие названия типов, даты и размеры: перечитываем, узнанное забываем
    connect(Language::notifier(), &LanguageNotifier::changed, this, [this] {
        stopSniff();
        m_sniffed.clear();
        m_resetNext = true;
        if (!m_path.isEmpty())
            reload();
        emit freeSpaceChanged();
    });
}

DirModel::~DirModel()
{
    stopTask(m_sniffTask);
    stopTask(m_searchTask);
    stopTask(m_listingTask);
    stopTask(m_sortTask);
    stopTask(m_countTask);
    if (m_eventFd >= 0) ::close(m_eventFd);
}

int DirModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant DirModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const FileEntry &e = m_rows.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case DirRole: return e.dir;
    case IsDirRole: return e.isDir;
    case IsHiddenRole: return e.isHidden;
    case IsLinkRole: return e.isLink;
    case IsLockedRole: return e.isLocked;
    case MtimeRole: return e.mtime.toSecsSinceEpoch();
    case SizeRole: return e.size;
    case SizeTextRole:
        if (!e.isDir)
            return formatSize(e.size);
        if (e.childCount < 0 && m_query.isEmpty())
            const_cast<DirModel *>(this)->queueCount(e);
        return e.childCount < 0 ? QString() : tr("%n эл.", nullptr, e.childCount);
    case DateTextRole: return formatDate(e.deleted.isValid() ? e.deleted : e.mtime);
    case MimeTypeRole: return e.mimeType;
    case MimeCommentRole: return e.mimeComment;
    case IconRole: return e.icon;
    case OriginRole: return e.origin;
    case GroupIdRole: return e.group;
    case GroupLabelRole: return m_groupBy.isEmpty() ? QString() : groupLabel(e.group);
    case FileIndexRole: return index.row();
    case HeaderRole: case PaddingRole: return false;
    }
    return {};
}

QHash<int, QByteArray> DirModel::roleNames() const { return fileRoleNames(); }

QHash<int, QByteArray> DirModel::fileRoleNames()
{
    return {
        {NameRole, "name"},         {PathRole, "path"},
        {DirRole, "dir"},           {IsDirRole, "isDir"},
        {IsHiddenRole, "isHidden"}, {IsLinkRole, "isLink"},
        {IsLockedRole, "isLocked"}, {MtimeRole, "mtime"},
        {SizeRole, "size"},         {SizeTextRole, "sizeText"},
        {DateTextRole, "dateText"}, {MimeTypeRole, "mimeType"},
        {MimeCommentRole, "mimeComment"}, {IconRole, "icon"},
        {OriginRole, "origin"}, {GroupLabelRole, "groupLabel"}, {GroupIdRole, "groupId"},
        {FileIndexRole, "fileIndex"}, {HeaderRole, "isHeader"}, {PaddingRole, "isPadding"},
    };
}

QString DirModel::expandPath(const QString &path)
{
    QString p = path.trimmed();
    if (p.startsWith(QLatin1String("file://")))
        p = QUrl(p).toLocalFile();
    if (p == QLatin1String("~") || p.startsWith(QLatin1String("~/")))
        p = QDir::homePath() + p.mid(1);
    return QDir::cleanPath(QDir(p).absolutePath());
}

void DirModel::setPath(const QString &path)
{
    const QString p = expandPath(path);
    const QFileInfo fi(p);
    const bool remote = isRemotePath(p);
    if (!remote && !fi.isDir()) {
        setError(tr("Нет такой папки: %1").arg(p));
        return;
    }
    if (!remote && !fi.isReadable()) {
        setError(tr("Нет доступа: %1").arg(p));
        return;
    }
    if (p == m_path && m_query.isEmpty())
        return;

    stopListing();
    stopCounts();
    m_reloadTimer.stop();
    m_path = p;
    watchDirectory();
    stopSniff();
    m_sniffed.clear();
    // Другая папка — не правка этой: строки по одной не меняем, иначе GridView играет
    // анимации удаления и вставки поверх перехода, и удалённые строки повисают призраками
    m_resetNext = true;
    m_all.clear();
    m_sourceIndex.clear();
    updateRows({});
    m_resetNext = true;
    emit countChanged();

    if (!m_query.isEmpty()) {
        stopSearch();
        m_query.clear();
        emit searchQueryChanged();
    }
    setError({});
    m_settingPath = true;
    emit pathChanged();
    m_settingPath = false;
    emit freeSpaceChanged();
    listDir();
}

void DirModel::setShowHidden(bool on)
{
    if (m_showHidden == on)
        return;
    m_showHidden = on;
    emit showHiddenChanged();
    if (m_query.isEmpty())
        applyFilterAndSort();
    else
        startSearch();
}

void DirModel::setSortKey(const QString &key) { setViewSort(key, m_sortDesc, m_groupBy, m_foldersFirst); }
void DirModel::setSortDesc(bool desc) { setViewSort(m_sortKey, desc, m_groupBy, m_foldersFirst); }
void DirModel::setFoldersFirst(bool on) { setViewSort(m_sortKey, m_sortDesc, m_groupBy, on); }
void DirModel::setGroupBy(const QString &key) { setViewSort(m_sortKey, m_sortDesc, key, m_foldersFirst); }
void DirModel::setViewSort(const QString &key, bool desc, const QString &group, bool foldersFirst)
{
    if (!QStringList{"name", "date", "size", "type", "ext", "created"}.contains(key)
        || !QStringList{"", "type", "date"}.contains(group)) return;
    if (m_sortKey == key && m_sortDesc == desc && m_groupBy == group && m_foldersFirst == foldersFirst) return;
    if (m_groupBy != group) m_resetNext = true;
    m_sortKey = key; m_sortDesc = desc; m_groupBy = group; m_foldersFirst = foldersFirst;
    emit sortChanged();
    if (!m_settingPath) applyFilterAndSort();
}

void DirModel::setSearchQuery(const QString &q)
{
    const QString query = q.trimmed();
    if (query == m_query)
        return;
    m_query = query;
    emit searchQueryChanged();
    if (m_query.isEmpty()) {
        stopSearch();
        listDir();
    } else {
        startSearch();
    }
}

void DirModel::setSearchOptions(const QVariantMap &options)
{
    if (m_searchOptions == options) return;
    m_searchOptions = options;
    emit searchOptionsChanged();
    if (!m_query.isEmpty()) startSearch();
}

void DirModel::reload()
{
    if (m_query.isEmpty())
        listDir();
    else
        startSearch();
}

void DirModel::watchDirectory()
{
    if (m_eventNotifier) {
        delete m_eventNotifier;
        m_eventNotifier = nullptr;
    }
    if (m_eventFd >= 0) { ::close(m_eventFd); m_eventFd = -1; }
    m_watch = -1;
    m_changedPaths.clear();
    m_fullReload = false;
    const auto watched = m_watcher.directories();
    if (!watched.isEmpty()) m_watcher.removePaths(watched);
    if (isRemotePath(m_path)) return;
    m_eventFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_eventFd >= 0) {
        m_watch = inotify_add_watch(m_eventFd, QFile::encodeName(m_path).constData(), IN_ONLYDIR | IN_CREATE | IN_DELETE
            | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT);
        if (m_watch >= 0) {
            m_eventNotifier = new QSocketNotifier(m_eventFd, QSocketNotifier::Read, this);
            connect(m_eventNotifier, &QSocketNotifier::activated, this, &DirModel::readDirectoryEvents);
            return;
        }
        ::close(m_eventFd);
        m_eventFd = -1;
    }
    m_watcher.addPath(m_path);
}

void DirModel::readDirectoryEvents()
{
    alignas(inotify_event) char buffer[16384];
    bool contentOnly = true;
    bool any = false;
    for (ssize_t bytes = ::read(m_eventFd, buffer, sizeof(buffer)); bytes > 0; bytes = ::read(m_eventFd, buffer, sizeof(buffer))) {
        for (ssize_t i = 0; i < bytes;) {
            const auto &event = *reinterpret_cast<const inotify_event *>(buffer + i);
            i += sizeof(inotify_event) + event.len;
            if (event.wd != m_watch && !(event.mask & IN_Q_OVERFLOW)) continue;
            any = true;
            if (!event.len || event.mask & IN_Q_OVERFLOW) { m_fullReload = true; contentOnly = false; continue; }
            if (event.mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) contentOnly = false;
            if (m_changedPaths.size() >= 64) m_fullReload = true;
            else m_changedPaths.insert(QDir(m_path).filePath(QFile::decodeName(event.name)));
        }
    }
    if (any && !m_reloadTimer.isActive()) m_reloadTimer.start(contentOnly ? 1000 : 200);
    else if (any && !contentOnly && m_reloadTimer.remainingTime() > 200) m_reloadTimer.start(200);
}

void DirModel::listDir(const QSet<QString> &changes)
{
    if (m_path.isEmpty())
        return;
    stopTask(m_listingTask);
    stopSniff();
    setLoading(true);
    const auto root = m_path;
    const bool inTrash = trashRoot();
    const bool fresh = m_all.isEmpty() && m_resetNext;
    auto *task = m_listingTask = new QFutureWatcher<Entries>(this);
    connect(task, &QFutureWatcher<Entries>::resultReadyAt, this, [this, task, changes](int i) {
        if (changes.isEmpty()) m_all = task->resultAt(i);
        else {
            m_all.removeIf([&](const FileEntry &e) { return changes.contains(e.path); });
            m_all.append(task->resultAt(i));
        }
        m_sourceIndex.clear();
        m_sourceIndex.reserve(m_all.size());
        for (int row = 0; row < m_all.size(); ++row) {
            FileEntry &e = m_all[row];
            m_sourceIndex.insert(e.path, row);
            if (e.typePending) useSniffed(e);
            const auto cached = m_counts.constFind(e.path);
            if (cached != m_counts.cend() && cached->first == e.mtime) e.childCount = cached->second;
        }
        applyFilterAndSort();
    });
    connect(task, &QFutureWatcher<Entries>::finished, this, [this, task] {
        m_listingTask = nullptr;
        task->deleteLater();
        Entries pending;
        for (const FileEntry &e : std::as_const(m_all))
            if (e.typePending) pending.append(e);
        if (!pending.isEmpty()) startSniff(pending);
        phaseFinished();
    });
    task->setFuture(QtConcurrent::run([root, inTrash, fresh, changes, collator = m_collator](QPromise<Entries> &promise) {
        const bool parentWritable = QFileInfo(root).isWritable();
        QMimeDatabase mime;
        Entries rows;
        QElapsedTimer budget;
        budget.start();
        auto append = [&](const QFileInfo &fi) {
            const auto typing = !changes.isEmpty() || budget.elapsed() < 16 ? FileEntry::Typing::ByName : FileEntry::Typing::Later;
            FileEntry e = FileEntry::fromInfo(fi, mime, false, typing, parentWritable);
            e.nameKey.emplace(collator.sortKey(e.name));
            if (inTrash) {
                const Trash::Info info = Trash::readInfo(e.path);
                e.origin = QFileInfo(info.origPath).absolutePath();
                e.deleted = info.deleted;
            }
            rows.append(std::move(e));
        };
        if (changes.isEmpty()) {
            QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (!promise.isCanceled() && it.hasNext()) {
                append(it.nextFileInfo());
                if (fresh && rows.size() == 500 && it.hasNext()) promise.addResult(rows);
            }
        } else {
            for (const auto &path : changes) {
                if (promise.isCanceled()) return;
                const QFileInfo fi(path);
                if (fi.exists() || fi.isSymLink()) append(fi);
            }
        }
        if (!promise.isCanceled()) promise.addResult(std::move(rows));
    }));
}

void DirModel::setLoading(bool loading)
{
    if (m_loading == loading) return;
    m_loading = loading;
    emit loadingChanged();
}

void DirModel::stopListing()
{
    stopTask(m_listingTask);
    const bool wasSorting = sorting();
    stopTask(m_sortTask);
    if (wasSorting) emit sortingChanged();
    setLoading(false);
}

void DirModel::phaseFinished()
{
    if (m_listingTask || m_sortTask) return;
    setLoading(false);
    emit reloaded();
}

void DirModel::queueCount(const FileEntry &entry)
{
    if (entry.childCount >= 0 || m_countRunning.contains(entry.path)) return;
    m_countQueue.insert(entry.path, entry.mtime);
    if (!m_countTask && !m_countTimer.isActive()) m_countTimer.start();
}

void DirModel::countFolders()
{
    if (m_countTask || m_countQueue.isEmpty()) return;
    auto paths = std::exchange(m_countQueue, {});
    m_countRunning = QSet<QString>(paths.keyBegin(), paths.keyEnd());
    auto *task = m_countTask = new QFutureWatcher<Entries>(this);
    connect(task, &QFutureWatcher<Entries>::resultReadyAt, this, [this, task](int i) {
        const auto batch = task->resultAt(i);
        for (const auto &entry : batch) {
            m_counts.insert(entry.path, {entry.mtime, entry.childCount});
            const int row = indexOfPath(entry.path);
            if (row >= 0 && m_rows.at(row).mtime == entry.mtime) {
                m_rows[row].childCount = entry.childCount;
                emit dataChanged(index(row), index(row), {SizeTextRole});
            }
        }
    });
    connect(task, &QFutureWatcher<Entries>::finished, this, [this, task] {
        m_countTask = nullptr;
        task->deleteLater();
        for (auto &entry : m_all) {
            const auto cached = m_counts.constFind(entry.path);
            if (cached != m_counts.cend() && cached->first == entry.mtime) entry.childCount = cached->second;
        }
        m_countRunning.clear();
        if (m_sortKey == QLatin1String("size") && !m_listingTask) applyFilterAndSort();
        if (!m_countQueue.isEmpty()) m_countTimer.start();
    });
    task->setFuture(QtConcurrent::run([paths = std::move(paths)](QPromise<Entries> &promise) {
        Entries batch;
        for (auto it = paths.cbegin(); it != paths.cend() && !promise.isCanceled(); ++it) {
            FileEntry entry;
            entry.path = it.key();
            entry.mtime = it.value();
            QDirIterator children(it.key(), QDir::AllEntries | QDir::NoDotAndDotDot);
            entry.childCount = 0;
            while (!promise.isCanceled() && children.hasNext()) {
                children.nextFileInfo();
                ++entry.childCount;
            }
            batch.append(std::move(entry));
            if (batch.size() >= 16) { promise.addResult(std::move(batch)); batch.clear(); }
        }
        if (!promise.isCanceled() && !batch.isEmpty()) promise.addResult(std::move(batch));
    }));
}

void DirModel::stopCounts()
{
    stopTask(m_countTask);
    m_countTimer.stop();
    m_countQueue.clear();
    m_countRunning.clear();
    m_counts.clear();
}

void DirModel::startSniff(const QList<FileEntry> &pending)
{
    auto *task = m_sniffTask = new QFutureWatcher<Entries>(this);
    connect(task, &QFutureWatcher<Entries>::resultReadyAt, this, [this, task](int i) { applySniffed(task->resultAt(i)); });
    connect(task, &QFutureWatcher<Entries>::finished, this, [this, task] {
        m_sniffTask = nullptr;
        task->deleteLater();
    });
    task->setFuture(QtConcurrent::run([pending](QPromise<Entries> &promise) {
        QMimeDatabase db;
        QList<FileEntry> batch;
        QElapsedTimer flushTimer;
        flushTimer.start();
        auto flush = [&] {
            if (batch.isEmpty())
                return;
            promise.addResult(std::move(batch));
            batch.clear();
            flushTimer.restart();
        };
        for (FileEntry e : pending) {
            if (promise.isCanceled())
                return;
            e.setMime(mimeForFile(QFileInfo(e.path), db));
            e.typePending = false;
            batch.append(e);
            if (flushTimer.elapsed() > 120)
                flush();
        }
        flush();
    }));
}

void DirModel::stopSniff()
{
    stopTask(m_sniffTask);
}

// Узнанный тип подходит, только если файл с тех пор не менялся
bool DirModel::useSniffed(FileEntry &e) const
{
    const auto it = m_sniffed.constFind(e.path);
    if (it == m_sniffed.cend() || it->mtime != e.mtime || it->size != e.size)
        return false;
    e.mimeType = it->mimeType;
    e.mimeComment = it->mimeComment;
    e.icon = it->icon;
    e.typePending = false;
    return true;
}

void DirModel::applySniffed(const QList<FileEntry> &batch)
{
    const bool layoutDependsOnType = !m_mimeFilters.isEmpty() || m_sortKey == QLatin1String("type") || m_groupBy == QLatin1String("type");
    QList<int> changed;
    for (const FileEntry &s : batch) {
        m_sniffed.insert(s.path, s);
        const auto source = m_sourceIndex.value(s.path, -1);
        if (source >= 0 && m_all.at(source).typePending) useSniffed(m_all[source]);
        if (layoutDependsOnType) continue;
        const int row = indexOfPath(s.path);
        if (row >= 0 && m_rows.at(row).typePending && useSniffed(m_rows[row])) changed << row;
    }
    if (layoutDependsOnType) {
        applyFilterAndSort();
        return;
    }
    if (sorting()) applyFilterAndSort();
    std::sort(changed.begin(), changed.end());
    for (int i = 0; i < changed.size();) {
        const int first = changed[i++];
        int last = first;
        while (i < changed.size() && changed[i] == last + 1) last = changed[i++];
        emit dataChanged(index(first), index(last), {MimeTypeRole, MimeCommentRole, IconRole});
    }
}

bool DirModel::lessThan(const FileEntry &a, const FileEntry &b) const
{
    return Ordering{m_sortKey, m_groupBy, m_sortDesc, m_foldersFirst, trashRoot(), m_collator}.less(a, b);
}

int DirModel::groupFor(const FileEntry &e, const QDate &today) const
{
    if (m_groupBy == QLatin1String("date")) {
        const auto date = (e.deleted.isValid() ? e.deleted : e.mtime).toLocalTime().date();
        if (!date.isValid()) return 5;
        if (date > today) return 0;
        if (date == today) return 1;
        if (date == today.addDays(-1)) return 2;
        if (date >= today.addDays(1 - today.dayOfWeek())) return 3;
        return 4;
    }
    if (e.isDir) return 0;
    const auto &m = e.mimeType;
    if (m.startsWith(QLatin1String("image/"))) return 1;
    if (m.startsWith(QLatin1String("audio/"))) return 2;
    if (m.startsWith(QLatin1String("video/"))) return 3;
    // Документы-контейнеры (ODF/OOXML/epub) не должны попасть в архивы.
    if (m.startsWith(QLatin1String("text/")) || m.contains(QLatin1String("pdf"))
        || m.contains(QLatin1String("opendocument")) || m.contains(QLatin1String("officedocument"))
        || m.contains(QLatin1String("msword")) || m.contains(QLatin1String("ms-excel"))
        || m.contains(QLatin1String("ms-powerpoint")) || m.contains(QLatin1String("epub"))) return 4;
    if (m.contains(QLatin1String("zip")) || m.contains(QLatin1String("tar"))
        || m.contains(QLatin1String("compressed")) || m.contains(QLatin1String("rar"))
        || m.contains(QLatin1String("7z")) || m.contains(QLatin1String("zstd"))
        || m == QLatin1String("application/x-xz")) return 5;
    return 6;
}

QString DirModel::groupLabel(int group) const
{
    if (m_groupBy == QLatin1String("date")) {
        const QStringList labels{tr("Будущие даты"), tr("Сегодня"), tr("Вчера"), tr("На этой неделе"), tr("Раньше"), tr("Без даты")};
        return labels.value(group);
    }
    const QStringList labels{tr("Папки"), tr("Изображения"), tr("Аудио"), tr("Видео"), tr("Документы"), tr("Архивы"), tr("Другие файлы")};
    return labels.value(group);
}

void DirModel::applyFilterAndSort()
{
    m_groupDate = QDate::currentDate();
    Entries rows;
    rows.reserve(m_all.size());
    for (const FileEntry &e : std::as_const(m_all)) {
        if ((m_showHidden || !e.isHidden) && passesFilters(e)) {
            rows.append(e);
            if (!m_groupBy.isEmpty()) rows.last().group = groupFor(e, m_groupDate);
            if (m_sortKey == QLatin1String("size") && e.isDir && m_query.isEmpty()) queueCount(e);
        }
    }
    const Ordering ordering{m_sortKey, m_groupBy, m_sortDesc, m_foldersFirst, trashRoot(), m_collator};
    const bool wasSorting = sorting();
    stopTask(m_sortTask);
    if (rows.size() <= 5000) {
        if (wasSorting) emit sortingChanged();
        present(ordering.sorted(std::move(rows)));
        phaseFinished();
        return;
    }
    auto *task = m_sortTask = new QFutureWatcher<Entries>(this);
    connect(task, &QFutureWatcher<Entries>::finished, this, [this, task] {
        m_sortTask = nullptr;
        present(task->result());
        task->deleteLater();
        emit sortingChanged();
        phaseFinished();
    });
    if (!wasSorting) emit sortingChanged();
    task->setFuture(QtConcurrent::run([rows = std::move(rows), ordering](QPromise<Entries> &promise) mutable {
        if (!promise.isCanceled()) promise.addResult(ordering.sorted(std::move(rows)));
    }));
}

void DirModel::present(const Entries &rows)
{
    updateRows(rows);
    emit countChanged();
}

// Изменения на диске применяем точечно: при сбросе модели QML пересоздаёт
// все строки, и поле переименования (или прокрутка) пропадает
void DirModel::updateRows(const QList<FileEntry> &rows)
{
    QHash<QString, int> newIndex;
    newIndex.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        newIndex.insert(rows.at(i).path, i);

    // Оставшиеся строки должны идти в том же порядке, иначе (сменилась
    // сортировка, файл переехал по дате) проще сбросить
    bool sameOrder = true;
    int last = -1;
    int kept = 0;
    for (const FileEntry &e : std::as_const(m_rows)) {
        const auto it = newIndex.constFind(e.path);
        if (it == newIndex.cend())
            continue;
        ++kept;
        if (*it <= last) sameOrder = false;
        last = *it;
    }
    // Много изменений (другая папка, конец поиска) — по одной строке дольше, чем сбросом
    const int changes = int(m_rows.size()) - kept + int(rows.size()) - kept;
    if (!m_resetNext && !sameOrder && changes == 0) {
        emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
        const auto previous = persistentIndexList();
        QModelIndexList replacements;
        replacements.reserve(previous.size());
        for (const auto &i : previous) replacements << index(newIndex.value(m_rows.at(i.row()).path));
        m_rows = rows;
        m_pathIndexDirty = true;
        changePersistentIndexList(previous, replacements);
        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        return;
    }
    if (m_resetNext || !sameOrder || m_rows.isEmpty() || changes > 64) {
        m_resetNext = false;
        beginResetModel();
        m_rows = rows;
        m_pathIndexDirty = true;
        endResetModel();
        return;
    }

    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (newIndex.contains(m_rows.at(i).path))
            continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        m_pathIndexDirty = true;
        endRemoveRows();
    }
    QList<int> roles;
    int firstChanged = -1;
    int lastChanged = -1;
    const auto notify = [&] {
        if (firstChanged >= 0) emit dataChanged(index(firstChanged), index(lastChanged), roles);
        firstChanged = -1;
    };
    for (int j = 0; j < rows.size(); ++j) {
        if (j < m_rows.size() && m_rows.at(j).path == rows.at(j).path) {
            const auto changed = changedRoles(m_rows.at(j), rows.at(j));
            m_rows[j] = rows.at(j);
            if (changed.isEmpty()) continue;
            if (firstChanged < 0 || j != lastChanged + 1 || changed != roles) {
                notify();
                firstChanged = j;
                roles = changed;
            }
            lastChanged = j;
            continue;
        }
        notify();
        beginInsertRows({}, j, j);
        m_rows.insert(j, rows.at(j));
        m_pathIndexDirty = true;
        endInsertRows();
    }
    notify();
}

void DirModel::stopSearch()
{
    stopTask(m_searchTask);
    if (m_searching) {
        m_searching = false;
        emit searchingChanged();
    }
}

void DirModel::startSearch()
{
    stopSearch();
    stopListing();
    stopSniff();
    beginResetModel();
    m_rows.clear();
    m_pathIndexDirty = true;
    m_all.clear();
    endResetModel();
    emit countChanged();

    m_searching = true;
    emit searchingChanged();

    const QString root = m_path;
    const QString query = m_query;
    const bool hidden = m_showHidden;
    const QString type = m_searchOptions.value(QStringLiteral("type")).toString();
    const qint64 min = m_searchOptions.value(QStringLiteral("min"), -1).toLongLong();
    const qint64 max = m_searchOptions.value(QStringLiteral("max"), -1).toLongLong();
    const int days = m_searchOptions.value(QStringLiteral("days")).toInt();
    const QDate earliest = days > 0 ? QDate::currentDate().addDays(1 - days) : QDate();
    const bool recursive = m_searchOptions.value(QStringLiteral("recursive"), true).toBool();
    auto *task = m_searchTask = new QFutureWatcher<Entries>(this);
    connect(task, &QFutureWatcher<Entries>::resultReadyAt, this, [this, task](int i) { appendResults(task->resultAt(i)); });
    connect(task, &QFutureWatcher<Entries>::finished, this, [this, task] {
        m_searchTask = nullptr;
        task->deleteLater();
        m_searching = false;
        emit searchingChanged();
    });

    // Обход в ширину: сначала ближние совпадения, глубокие — потом
    task->setFuture(QtConcurrent::run([root, query, hidden, type, min, max, earliest, recursive](QPromise<Entries> &promise) {
        QMimeDatabase db;
        QList<FileEntry> batch;
        QElapsedTimer flushTimer;
        flushTimer.start();
        int found = 0;
        QQueue<QString> queue;
        queue.enqueue(root);

        auto flush = [&] {
            if (batch.isEmpty())
                return;
            promise.addResult(std::move(batch));
            batch.clear();
            flushTimer.restart();
        };

        while (!queue.isEmpty() && !promise.isCanceled() && found < kSearchLimit) {
            const QString dir = queue.dequeue();
            const bool parentWritable = QFileInfo(dir).isWritable();
            QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (!promise.isCanceled() && it.hasNext()) {
                const QFileInfo fi = it.nextFileInfo();
                const QString name = fi.fileName();
                if (!hidden && name.startsWith(QLatin1Char('.')))
                    continue;
                if (recursive && fi.isDir() && !fi.isSymLink() && !skipInSearch(name, fi.absoluteFilePath()))
                    queue.enqueue(fi.absoluteFilePath());
                if (name.contains(query, Qt::CaseInsensitive)
                    && (!earliest.isValid() || fi.lastModified().date() >= earliest)
                    && (min < 0 || (!fi.isDir() && fi.size() > min))
                    && (max < 0 || (!fi.isDir() && fi.size() < max))) {
                    FileEntry entry = FileEntry::fromInfo(fi, db, false, FileEntry::Typing::Full, parentWritable);
                    const auto m = entry.mimeType;
                    const bool matches = type.isEmpty() || m.startsWith(type + QLatin1Char('/'))
                        || (type == QLatin1String("folder") && entry.isDir)
                        || (type == QLatin1String("document") && (m.startsWith(QLatin1String("text/"))
                            || m.contains(QLatin1String("pdf")) || m.contains(QLatin1String("opendocument"))
                            || m.contains(QLatin1String("officedocument")) || m.contains(QLatin1String("msword"))
                            || m.endsWith(QLatin1String("/rtf"))))
                        || (type == QLatin1String("archive") && (m.contains(QLatin1String("zip"))
                            || m.contains(QLatin1String("tar")) || m.contains(QLatin1String("compressed"))
                            || m.contains(QLatin1String("rar")) || m.contains(QLatin1String("7z"))));
                    if (!matches) continue;
                    batch.append(std::move(entry));
                    if (++found >= kSearchLimit)
                        break;
                }
                if (batch.size() >= 128 || flushTimer.elapsed() > 120)
                    flush();
            }
        }
        flush();
    }));
}

void DirModel::appendResults(const QList<FileEntry> &batch)
{
    m_all.append(batch);
    if (!m_groupBy.isEmpty()) {
        applyFilterAndSort();
        return;
    }
    // Поиск приходит порциями: сохраняем порядок обхода и не сортируем весь
    // накопленный результат при каждом обновлении.
    const int first = int(m_rows.size());
    beginInsertRows({}, first, first + int(batch.size()) - 1);
    m_rows.append(batch);
    m_pathIndexDirty = true;
    endInsertRows();
    emit countChanged();
}

void DirModel::setError(const QString &e)
{
    if (m_error == e)
        return;
    m_error = e;
    emit errorChanged();
}

int DirModel::indexOfPath(const QString &path) const
{
    if (m_pathIndexDirty) {
        m_pathIndex.clear();
        m_pathIndex.reserve(m_rows.size());
        for (int i = 0; i < m_rows.size(); ++i)
            m_pathIndex.insert(m_rows.at(i).path, i);
        m_pathIndexDirty = false;
    }
    return m_pathIndex.value(path, -1);
}

int DirModel::indexOfPrefix(const QString &prefix) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).name.startsWith(prefix, Qt::CaseInsensitive)) return i;
    return -1;
}

QString DirModel::pathAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).path : QString();
}

bool DirModel::isDirAt(int row) const
{
    return row >= 0 && row < m_rows.size() && m_rows.at(row).isDir;
}

QString DirModel::iconAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).icon : QString();
}

QString DirModel::mimeTypeAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).mimeType : QString();
}

QString DirModel::mimeCommentAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).mimeComment : QString();
}

QVariantMap DirModel::rowInfo(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_rows.size())
        return m;
    const QModelIndex i = index(row);
    const auto roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        m[QString::fromLatin1(it.value())] = data(i, it.key());
    return m;
}

QString DirModel::sizeOf(const QStringList &paths) const
{
    qint64 total = 0;
    bool onlyFiles = true;
    for (const QString &p : paths) {
        const int i = indexOfPath(p);
        if (i < 0)
            continue;
        if (m_rows.at(i).isDir)
            onlyFiles = false;
        else
            total += m_rows.at(i).size;
    }
    return onlyFiles && total > 0 ? formatSize(total) : QString();
}

bool DirModel::passesFilters(const FileEntry &e) const
{
    if (e.isDir)
        return true;
    if (m_dirsOnly)
        return false;
    if (m_nameRegex.isEmpty() && m_mimeFilters.isEmpty())
        return true;
    for (const QRegularExpression &re : m_nameRegex)
        if (re.match(e.name).hasMatch())
            return true;
    if (!m_mimeFilters.isEmpty()) {
        const QMimeType mt = m_mime.mimeTypeForName(e.mimeType);
        for (const QString &m : m_mimeFilters)
            // image/* и точное совпадение; inherits — text/x-c++src подходит под text/plain
            if ((m.endsWith(QLatin1String("/*")) && e.mimeType.startsWith(m.chopped(1))) || mt.inherits(m))
                return true;
    }
    return false;
}

void DirModel::setNameFilters(const QStringList &globs)
{
    if (globs == m_nameFilters)
        return;
    m_nameFilters = globs;
    m_nameRegex.clear();
    for (const QString &g : globs)
        m_nameRegex << QRegularExpression(QRegularExpression::wildcardToRegularExpression(g),
                                          QRegularExpression::CaseInsensitiveOption);
    emit filtersChanged();
    refilter();
}

void DirModel::setMimeFilters(const QStringList &mimes)
{
    if (mimes == m_mimeFilters)
        return;
    m_mimeFilters = mimes;
    emit filtersChanged();
    refilter();
}

void DirModel::setDirsOnly(bool on)
{
    if (on == m_dirsOnly)
        return;
    m_dirsOnly = on;
    emit filtersChanged();
    refilter();
}

// Другой фильтр — как другая папка: окно выбора меняет его по нескольку свойств подряд,
// и построчные анимации одного шага обрываются следующим, оставляя призраков в сетке
void DirModel::refilter()
{
    m_resetNext = true;
    if (!m_all.isEmpty()) applyFilterAndSort();
}

bool DirModel::trashRoot() const
{
    return m_path == Trash::filesDir();
}

QString DirModel::freeSpace() const
{
    if (m_path.isEmpty() || isRemotePath(m_path)) return {};
    const QStorageInfo si(m_path);
    if (!si.isValid())
        return {};
    return tr("свободно %1").arg(formatSize(si.bytesAvailable()));
}
