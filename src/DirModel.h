#pragma once

#include "FileEntry.h"

#include <QAbstractListModel>
#include <QCollator>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

// Содержимое одной папки или результаты поиска в ней (searchQuery не пуст).
class DirModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
    Q_PROPERTY(bool sortDesc READ sortDesc WRITE setSortDesc NOTIFY sortChanged)
    Q_PROPERTY(bool foldersFirst READ foldersFirst WRITE setFoldersFirst NOTIFY sortChanged)
    Q_PROPERTY(QString groupBy READ groupBy WRITE setGroupBy NOTIFY sortChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(QVariantMap searchOptions READ searchOptions WRITE setSearchOptions NOTIFY searchOptionsChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool sorting READ sorting NOTIFY sortingChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString freeSpace READ freeSpace NOTIFY freeSpaceChanged)
    // Открыт корень корзины: вместо типа — «Откуда», вместо даты изменения — дата удаления
    Q_PROPERTY(bool trashRoot READ trashRoot NOTIFY pathChanged)
    // Окно выбора файлов: показывать только подходящие файлы (папки видны всегда)
    Q_PROPERTY(QStringList nameFilters READ nameFilters WRITE setNameFilters NOTIFY filtersChanged)
    Q_PROPERTY(QStringList mimeFilters READ mimeFilters WRITE setMimeFilters NOTIFY filtersChanged)
    Q_PROPERTY(bool dirsOnly READ dirsOnly WRITE setDirsOnly NOTIFY filtersChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        DirRole,
        IsDirRole,
        IsHiddenRole,
        IsLinkRole,
        IsLockedRole,
        MtimeRole,
        SizeRole,
        SizeTextRole,
        DateTextRole,
        MimeTypeRole,
        MimeCommentRole,
        IconRole,
        OriginRole,
        GroupLabelRole,
        GroupIdRole,
        FileIndexRole,
        HeaderRole,
        PaddingRole,
    };

    explicit DirModel(QObject *parent = nullptr);
    ~DirModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    static QHash<int, QByteArray> fileRoleNames();

    QString path() const { return m_path; }
    void setPath(const QString &path);
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool on);
    QString sortKey() const { return m_sortKey; }
    void setSortKey(const QString &key);
    bool sortDesc() const { return m_sortDesc; }
    void setSortDesc(bool desc);
    bool foldersFirst() const { return m_foldersFirst; }
    void setFoldersFirst(bool on);
    QString groupBy() const { return m_groupBy; }
    void setGroupBy(const QString &key);
    Q_INVOKABLE void setViewSort(const QString &key, bool desc, const QString &group, bool foldersFirst);
    QString searchQuery() const { return m_query; }
    void setSearchQuery(const QString &q);
    QVariantMap searchOptions() const { return m_searchOptions; }
    void setSearchOptions(const QVariantMap &options);
    bool searching() const { return m_searching; }
    bool loading() const { return m_loading; }
    bool sorting() const { return m_sortTask != nullptr; }
    int count() const { return int(m_rows.size()); }
    QString error() const { return m_error; }
    QString freeSpace() const;
    bool trashRoot() const;
    QStringList nameFilters() const { return m_nameFilters; }
    void setNameFilters(const QStringList &globs);
    QStringList mimeFilters() const { return m_mimeFilters; }
    void setMimeFilters(const QStringList &mimes);
    bool dirsOnly() const { return m_dirsOnly; }
    void setDirsOnly(bool on);

    Q_INVOKABLE void reload();
    Q_INVOKABLE int indexOfPath(const QString &path) const;
    Q_INVOKABLE int indexOfPrefix(const QString &prefix) const;
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE bool isDirAt(int row) const;
    Q_INVOKABLE QString iconAt(int row) const;
    Q_INVOKABLE QString mimeTypeAt(int row) const;
    Q_INVOKABLE QString mimeCommentAt(int row) const;
    // Всё о строке сразу, по именам ролей (name, sizeText, dateText…) — для быстрого просмотра
    Q_INVOKABLE QVariantMap rowInfo(int row) const;
    Q_INVOKABLE QString sizeOf(const QStringList &paths) const;
    Q_INVOKABLE static QString expandPath(const QString &path);

signals:
    void pathChanged();
    // Другая папка или другой язык
    void freeSpaceChanged();
    void showHiddenChanged();
    void sortChanged();
    void searchQueryChanged();
    void searchOptionsChanged();
    void searchingChanged();
    void loadingChanged();
    void sortingChanged();
    void countChanged();
    void errorChanged();
    // Список перечитан (смена папки, изменения на диске) — QML восстанавливает выделение
    void reloaded();
    void filtersChanged();

private:
    friend class TestCore;
    void listDir(const QSet<QString> &changes = {});
    void watchDirectory();
    void readDirectoryEvents();
    void stopListing();
    void setLoading(bool loading);
    int groupFor(const FileEntry &entry, const QDate &today) const;
    QString groupLabel(int group) const;
    void applyFilterAndSort();
    void present(const QList<FileEntry> &rows);
    void phaseFinished();
    void queueCount(const FileEntry &entry);
    void countFolders();
    void stopCounts();
    void refilter();
    void updateRows(const QList<FileEntry> &rows);
    void startSearch();
    void stopSearch();
    void appendResults(const QList<FileEntry> &batch);
    void startSniff(const QList<FileEntry> &pending);
    void stopSniff();
    void applySniffed(const QList<FileEntry> &batch);
    bool useSniffed(FileEntry &e) const;
    void setError(const QString &e);
    bool lessThan(const FileEntry &a, const FileEntry &b) const;
    bool passesFilters(const FileEntry &e) const;

    QString m_path;
    QString m_query;
    QVariantMap m_searchOptions;
    QString m_error;
    QString m_sortKey = QStringLiteral("name");
    bool m_sortDesc = false;
    bool m_foldersFirst = true;
    QString m_groupBy;
    QDate m_groupDate;
    QTimer m_dateTimer;
    bool m_showHidden = false;
    bool m_searching = false;
    bool m_loading = false;
    QStringList m_nameFilters;
    QList<QRegularExpression> m_nameRegex;
    QStringList m_mimeFilters;
    bool m_dirsOnly = false;
    // Сменилась папка — следующее перечитывание перестраивает список целиком
    bool m_resetNext = true;
    bool m_settingPath = false;

    QList<FileEntry> m_all;   // всё содержимое папки, со скрытыми
    QHash<QString, int> m_sourceIndex;
    QList<FileEntry> m_rows;  // то, что видно
    mutable QHash<QString, int> m_pathIndex;
    mutable bool m_pathIndexDirty = true;

    QMimeDatabase m_mime;
    QCollator m_collator;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    QSocketNotifier *m_eventNotifier = nullptr;
    int m_eventFd = -1;
    int m_watch = -1;
    QSet<QString> m_changedPaths;
    bool m_fullReload = false;

    using Entries = QList<FileEntry>;
    QFutureWatcher<Entries> *m_searchTask = nullptr;
    QFutureWatcher<Entries> *m_listingTask = nullptr;
    QFutureWatcher<Entries> *m_sortTask = nullptr;
    QFutureWatcher<Entries> *m_countTask = nullptr;
    QHash<QString, QPair<QDateTime, int>> m_counts;
    QHash<QString, QDateTime> m_countQueue;
    QSet<QString> m_countRunning;
    QTimer m_countTimer;

    // Что не успели или не стали определять при листинге (FileEntry::typePending),
    // определяется в фоне; узнанное помним до смены папки, чтобы не определять заново
    QHash<QString, FileEntry> m_sniffed;
    QFutureWatcher<Entries> *m_sniffTask = nullptr;
};
