#include "DirModel.h"

#include "Language.h"
#include "Trash.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QQueue>
#include <QStorageInfo>
#include <QtConcurrent>

#include <algorithm>

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

    // Копирование большой папки шлёт сотни событий — перечитываем не чаще раза в 200 мс
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(200);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        if (m_query.isEmpty())
            listDir();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
    // Другой язык — другие названия типов, даты и размеры: перечитываем, узнанное забываем
    connect(Language::notifier(), &LanguageNotifier::changed, this, [this] {
        stopSniff();
        m_sniffed.clear();
        if (!m_path.isEmpty())
            reload();
        emit freeSpaceChanged();
    });
}

DirModel::~DirModel()
{
    stopSniff();
    stopSearch();
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
    if (!fi.isDir()) {
        setError(tr("Нет такой папки: %1").arg(p));
        return;
    }
    if (!fi.isReadable()) {
        setError(tr("Нет доступа: %1").arg(p));
        return;
    }
    if (p == m_path && m_query.isEmpty())
        return;

    if (!m_path.isEmpty())
        m_watcher.removePath(m_path);
    m_path = p;
    m_watcher.addPath(m_path);
    stopSniff();
    m_sniffed.clear();
    // Другая папка — не правка этой: строки по одной не меняем, иначе GridView играет
    // анимации удаления и вставки поверх перехода, и удалённые строки повисают призраками
    m_resetNext = true;

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

void DirModel::reload()
{
    if (m_query.isEmpty())
        listDir();
    else
        startSearch();
}

void DirModel::listDir()
{
    if (m_path.isEmpty())
        return;
    stopSniff();
    const QDir dir(m_path);
    const QFileInfoList infos = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    m_all.clear();
    m_all.reserve(infos.size());
    QList<FileEntry> pending;
    const bool inTrash = trashRoot();
    // Типы — сколько успеем за кадр (16 мс), остальные доопределит фон:
    // обычная папка выглядит сразу как надо, огромная не замирает
    QElapsedTimer budget;
    budget.start();
    for (const QFileInfo &fi : infos) {
        FileEntry e = FileEntry::fromInfo(fi, m_mime, true,
                                          budget.elapsed() < 16 ? FileEntry::Typing::ByName : FileEntry::Typing::Later);
        if (e.typePending && !useSniffed(e))
            pending.append(e);
        if (inTrash) {
            const Trash::Info info = Trash::readInfo(e.path);
            e.origin = QFileInfo(info.origPath).absolutePath();
            e.deleted = info.deleted;
        }
        m_all.append(e);
    }
    applyFilterAndSort();
    if (!pending.isEmpty())
        startSniff(pending);
}

void DirModel::startSniff(const QList<FileEntry> &pending)
{
    const quint64 gen = m_sniffGen;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_sniffCancel = cancel;
    m_sniffFuture = QtConcurrent::run([this, gen, cancel, pending] {
        QMimeDatabase db;
        QList<FileEntry> batch;
        QElapsedTimer flushTimer;
        flushTimer.start();
        auto flush = [&] {
            if (batch.isEmpty())
                return;
            QMetaObject::invokeMethod(this, [this, gen, b = std::move(batch)] { applySniffed(gen, b); },
                                      Qt::QueuedConnection);
            batch.clear();
            flushTimer.restart();
        };
        for (FileEntry e : pending) {
            if (cancel->load())
                return;
            e.setMime(mimeForFile(QFileInfo(e.path), db));
            e.typePending = false;
            batch.append(e);
            if (flushTimer.elapsed() > 120)
                flush();
        }
        flush();
    });
}

void DirModel::stopSniff()
{
    ++m_sniffGen;
    if (m_sniffCancel)
        m_sniffCancel->store(true);
    if (m_sniffFuture.isRunning())
        m_sniffFuture.waitForFinished();
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

void DirModel::applySniffed(quint64 gen, const QList<FileEntry> &batch)
{
    if (gen != m_sniffGen)
        return;
    for (const FileEntry &s : batch)
        m_sniffed.insert(s.path, s);
    for (FileEntry &e : m_all)
        if (e.typePending)
            useSniffed(e);
    // От типа зависят фильтры окна выбора и сортировка по типу — тогда список перестраивается
    if (!m_mimeFilters.isEmpty() || m_sortKey == QLatin1String("type") || m_groupBy == QLatin1String("type")) {
        applyFilterAndSort();
        return;
    }
    int first = -1, last = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (!m_rows.at(i).typePending || !useSniffed(m_rows[i]))
            continue;
        if (first < 0)
            first = i;
        last = i;
    }
    if (first >= 0)
        emit dataChanged(index(first), index(last), {MimeTypeRole, MimeCommentRole, IconRole});
}

bool DirModel::lessThan(const FileEntry &a, const FileEntry &b) const
{
    if (!m_groupBy.isEmpty() && a.group != b.group)
        return m_sortDesc ? a.group > b.group : a.group < b.group;
    if (m_foldersFirst && a.isDir != b.isDir)
        return a.isDir; // папки всегда сверху, независимо от направления
    int c = 0;
    if (m_sortKey == QLatin1String("size"))
        c = a.isDir && b.isDir ? (a.childCount - b.childCount) : (a.size < b.size ? -1 : a.size > b.size ? 1 : 0);
    else if (m_sortKey == QLatin1String("ext"))
        c = QString::compare(a.extension, b.extension, Qt::CaseInsensitive);
    else if (m_sortKey == QLatin1String("created"))
        c = a.created < b.created ? -1 : a.created > b.created ? 1 : 0;
    else if (m_sortKey == QLatin1String("date")) {
        const QDateTime &x = a.deleted.isValid() ? a.deleted : a.mtime;
        const QDateTime &y = b.deleted.isValid() ? b.deleted : b.mtime;
        c = x < y ? -1 : x > y ? 1 : 0;
    } else if (m_sortKey == QLatin1String("type") && trashRoot())
        c = m_collator.compare(a.origin, b.origin);
    else if (m_sortKey == QLatin1String("type"))
        c = m_collator.compare(a.mimeComment, b.mimeComment);
    if (c == 0)
        c = m_collator.compare(a.name, b.name);
    return m_sortDesc ? c > 0 : c < 0;
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
    QList<FileEntry> rows;
    for (const FileEntry &e : std::as_const(m_all)) {
        if ((m_showHidden || !e.isHidden) && passesFilters(e)) {
            rows.append(e);
            if (!m_groupBy.isEmpty()) rows.last().group = groupFor(e, m_groupDate);
        }
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [this](const FileEntry &a, const FileEntry &b) { return lessThan(a, b); });
    updateRows(rows);
    emit countChanged();
    emit reloaded();
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
        if (*it <= last) {
            sameOrder = false;
            break;
        }
        last = *it;
    }
    // Много изменений (другая папка, конец поиска) — по одной строке дольше, чем сбросом
    const int changes = int(m_rows.size()) - kept + int(rows.size()) - kept;
    if (m_resetNext || !sameOrder || m_rows.isEmpty() || changes > 64) {
        m_resetNext = false;
        beginResetModel();
        m_rows = rows;
        endResetModel();
        return;
    }

    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (newIndex.contains(m_rows.at(i).path))
            continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }
    for (int j = 0; j < rows.size(); ++j) {
        if (j < m_rows.size() && m_rows.at(j).path == rows.at(j).path) {
            m_rows[j] = rows.at(j);
            continue;
        }
        beginInsertRows({}, j, j);
        m_rows.insert(j, rows.at(j));
        endInsertRows();
    }
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1));
}

void DirModel::stopSearch()
{
    ++m_searchGen;
    if (m_searchCancel)
        m_searchCancel->store(true);
    if (m_searchFuture.isRunning())
        m_searchFuture.waitForFinished();
    if (m_searching) {
        m_searching = false;
        emit searchingChanged();
    }
}

void DirModel::startSearch()
{
    stopSearch();
    beginResetModel();
    m_rows.clear();
    m_all.clear();
    endResetModel();
    emit countChanged();

    const quint64 gen = m_searchGen;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_searchCancel = cancel;
    m_searching = true;
    emit searchingChanged();

    const QString root = m_path;
    const QString query = m_query;
    const bool hidden = m_showHidden;

    // Обход в ширину: сначала ближние совпадения, глубокие — потом
    m_searchFuture = QtConcurrent::run([this, gen, cancel, root, query, hidden] {
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
            QMetaObject::invokeMethod(this, [this, gen, b = std::move(batch)] { appendResults(gen, b); },
                                      Qt::QueuedConnection);
            batch.clear();
            flushTimer.restart();
        };

        while (!queue.isEmpty() && !cancel->load() && found < kSearchLimit) {
            const QString dir = queue.dequeue();
            QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (it.hasNext() && !cancel->load()) {
                const QFileInfo fi = it.nextFileInfo();
                const QString name = fi.fileName();
                if (!hidden && name.startsWith(QLatin1Char('.')))
                    continue;
                if (fi.isDir() && !fi.isSymLink() && !skipInSearch(name, fi.absoluteFilePath()))
                    queue.enqueue(fi.absoluteFilePath());
                if (name.contains(query, Qt::CaseInsensitive)) {
                    batch.append(FileEntry::fromInfo(fi, db, false));
                    if (++found >= kSearchLimit)
                        break;
                }
                if (flushTimer.elapsed() > 120)
                    flush();
            }
        }
        flush();
        QMetaObject::invokeMethod(this, [this, gen] {
            if (gen != m_searchGen || !m_searching)
                return;
            m_searching = false;
            emit searchingChanged();
        }, Qt::QueuedConnection);
    });
}

void DirModel::appendResults(quint64 gen, const QList<FileEntry> &batch)
{
    if (gen != m_searchGen)
        return;
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
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).path == path)
            return i;
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
    reload();
}

bool DirModel::trashRoot() const
{
    return m_path == Trash::filesDir();
}

QString DirModel::freeSpace() const
{
    const QStorageInfo si(m_path);
    if (!si.isValid())
        return {};
    return tr("свободно %1").arg(formatSize(si.bytesAvailable()));
}
