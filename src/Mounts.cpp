#include <gio/gio.h>

#include "Mounts.h"

#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

namespace {
QString takeString(char *value)
{
    const auto result = QString::fromUtf8(value);
    g_free(value);
    return result;
}

struct Unref { void operator()(void *value) const { g_object_unref(value); } };
template <typename T> using Ref = std::unique_ptr<T, Unref>;

QString deviceId(GVolume *volume)
{
    return takeString(g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
}

QString fileUri(GFile *file) { return takeString(g_file_get_uri(file)); }
}

struct Mounts::State {
    struct Job {
        QPointer<Mounts> owner;
        int id;
        Ref<GMountOperation> operation{g_mount_operation_new()};
        Ref<GCancellable> cancellable{g_cancellable_new()};
        bool awaiting = false;
    };

    Mounts *owner;
    Ref<GVolumeMonitor> monitor;
    QHash<int, Job *> jobs;
    QVariantList items;
    int serial = 0;

    void init()
    {
        if (monitor) return;
        monitor.reset(g_volume_monitor_get());
        for (const char *signal : {"mount-added", "mount-removed", "mount-changed", "volume-added", "volume-removed", "volume-changed"}) {
            g_signal_connect(monitor.get(), signal, G_CALLBACK(+[](GVolumeMonitor *, GObject *, gpointer data) {
                static_cast<State *>(data)->refresh();
            }), this);
        }
        refresh();
    }

    void refresh()
    {
        QVariantList result;
        auto *volumes = g_volume_monitor_get_volumes(monitor.get());
        for (auto *entry = volumes; entry; entry = entry->next) {
            auto *volume = G_VOLUME(entry->data);
            Ref<GMount> mount(g_volume_get_mount(volume));
            if (mount || !g_volume_can_mount(volume)) continue;
            Ref<GFile> root(g_volume_get_activation_root(volume));
            const QString device = deviceId(volume);
            const QString uri = root ? fileUri(root.get()) : QString();
            const bool remote = Mounts::remoteUrl(uri);
            if (!remote && device.isEmpty()) continue;
            result << QVariantMap{{"path", remote ? uri : QStringLiteral("device:") + device},
                {"name", takeString(g_volume_get_name(volume))}, {"icon", remote ? "drive" : "usb"}, {"unmounted", true}};
        }
        g_list_free_full(volumes, g_object_unref);
        auto *mounts = g_volume_monitor_get_mounts(monitor.get());
        for (auto *entry = mounts; entry; entry = entry->next) {
            auto *mount = G_MOUNT(entry->data);
            Ref<GFile> root(g_mount_get_root(mount));
            const QString uri = fileUri(root.get());
            if (!Mounts::remoteUrl(uri)) continue;
            result << QVariantMap{{"path", uri}, {"name", takeString(g_mount_get_name(mount))}, {"icon", "drive"},
                {"removable", g_mount_can_unmount(mount) != 0}, {"ejectable", g_mount_can_eject(mount) != 0}};
        }
        g_list_free_full(mounts, g_object_unref);
        if (result != items) { items = result; emit owner->devicesChanged(); }
    }

    Job *start()
    {
        auto *job = new Job{owner, ++serial};
        jobs.insert(job->id, job);
        g_signal_connect(job->operation.get(), "ask-password", G_CALLBACK((+[](GMountOperation *operation, const char *message,
            const char *user, const char *domain, GAskPasswordFlags flags, gpointer data) {
            auto *job = static_cast<Job *>(data);
            g_signal_stop_emission_by_name(operation, "ask-password");
            job->awaiting = true;
            if (job->owner) emit job->owner->prompt(job->id, {{"text", QString::fromUtf8(message)},
                {"user", QString::fromUtf8(user)}, {"domain", QString::fromUtf8(domain)},
                {"needUser", bool(flags & G_ASK_PASSWORD_NEED_USERNAME)}, {"needDomain", bool(flags & G_ASK_PASSWORD_NEED_DOMAIN)},
                {"needPassword", bool(flags & G_ASK_PASSWORD_NEED_PASSWORD)}, {"anonymous", bool(flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED)}});
        })), job);
        g_signal_connect(job->operation.get(), "ask-question", G_CALLBACK((+[](GMountOperation *operation, const char *message,
            const char **choices, gpointer data) {
            auto *job = static_cast<Job *>(data);
            g_signal_stop_emission_by_name(operation, "ask-question");
            job->awaiting = true;
            QStringList labels;
            while (*choices) labels << QString::fromUtf8(*choices++);
            if (job->owner) emit job->owner->prompt(job->id, {{"text", QString::fromUtf8(message)}, {"choices", labels}});
        })), job);
        return job;
    }

    static void complete(Job *job, GError *error = nullptr, GFile *root = nullptr)
    {
        const auto owner = job->owner;
        const int id = job->id;
        const bool cancelled = g_cancellable_is_cancelled(job->cancellable.get());
        if (owner) owner->m_state->jobs.remove(id);
        delete job;
        const QString message = error ? QString::fromUtf8(error->message) : QString();
        g_clear_error(&error);
        if (!owner || cancelled) return;
        if (!root || !message.isEmpty()) {
            QMetaObject::invokeMethod(owner, [owner, id, message] { if (owner) emit owner->finished(id, {}, message); }, Qt::QueuedConnection);
            return;
        }
        auto *watcher = new QFutureWatcher<QString>(owner);
        connect(watcher, &QFutureWatcher<QString>::finished, owner, [owner, watcher, id] {
            const QString path = watcher->result();
            watcher->deleteLater();
            emit owner->finished(id, path, path.isEmpty() ? tr("Нет доступа через GVfs FUSE. Проверьте установку GVfs.") : QString());
        });
        auto file = std::shared_ptr<GFile>(G_FILE(g_object_ref(root)), Unref{});
        watcher->setFuture(QtConcurrent::run([file] { return takeString(g_file_get_path(file.get())); }));
    }
};

Mounts::Mounts(QObject *parent) : QObject(parent), m_state(std::make_unique<State>())
{
    m_state->owner = this;
    QTimer::singleShot(250, this, [this] { m_state->init(); });
}

Mounts::~Mounts()
{
    if (m_state->monitor) g_signal_handlers_disconnect_by_data(m_state->monitor.get(), m_state.get());
    for (auto *job : std::as_const(m_state->jobs)) {
        job->owner.clear();
        g_cancellable_cancel(job->cancellable.get());
        if (job->awaiting) g_mount_operation_reply(job->operation.get(), G_MOUNT_OPERATION_ABORTED);
    }
}

QVariantList Mounts::devices() const { return m_state->items; }

bool Mounts::remoteUrl(const QString &path)
{
    return path.startsWith(QLatin1String("smb://"), Qt::CaseInsensitive)
        || path.startsWith(QLatin1String("sftp://"), Qt::CaseInsensitive)
        || path.startsWith(QLatin1String("mtp://"), Qt::CaseInsensitive)
        || path.startsWith(QLatin1String("afc://"), Qt::CaseInsensitive);
}

bool Mounts::networkAvailable() const
{
    const auto schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
    return schemes && (g_strv_contains(schemes, "smb") || g_strv_contains(schemes, "sftp") || g_strv_contains(schemes, "mtp"));
}

int Mounts::open(const QString &location)
{
    m_state->init();
    auto *job = m_state->start();
    const int id = job->id;
    auto *op = job->operation.get();
    auto *cancel = job->cancellable.get();
    if (location.startsWith(QLatin1String("device:"))) {
        auto *volumes = g_volume_monitor_get_volumes(m_state->monitor.get());
        bool found = false;
        for (auto *entry = volumes; entry; entry = entry->next) {
            auto *volume = G_VOLUME(entry->data);
            if (deviceId(volume) != location.mid(7)) continue;
            found = true;
            g_volume_mount(volume, G_MOUNT_MOUNT_NONE, op, cancel, +[](GObject *obj, GAsyncResult *r, gpointer data) {
                GError *error = nullptr;
                auto *volume = G_VOLUME(obj);
                g_volume_mount_finish(volume, r, &error);
                Ref<GMount> mount(g_volume_get_mount(volume));
                Ref<GFile> root(mount ? g_mount_get_default_location(mount.get()) : nullptr);
                State::complete(static_cast<State::Job *>(data), error, root.get());
            }, job);
            break;
        }
        g_list_free_full(volumes, g_object_unref);
        if (found) return id;
    } else if (remoteUrl(location)) {
        const QString scheme = location.section(QLatin1Char(':'), 0, 0).toLower();
        const auto schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
        if (schemes && g_strv_contains(schemes, scheme.toUtf8().constData())) {
            // MTP uses addresses such as mtp://[usb:001,003]/, which QUrl treats as invalid IPv6.
            QByteArray uri = location.toUtf8();
            if (scheme == QLatin1String("smb") || scheme == QLatin1String("sftp")) {
                auto url = QUrl(location);
                url.setPassword({});
                uri = url.toEncoded();
            }
            Ref<GFile> file(g_file_new_for_uri(uri.constData()));
            g_file_mount_enclosing_volume(file.get(), G_MOUNT_MOUNT_NONE, op, cancel, +[](GObject *obj, GAsyncResult *r, gpointer data) {
                GError *error = nullptr;
                auto *file = G_FILE(obj);
                g_file_mount_enclosing_volume_finish(file, r, &error);
                if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) g_clear_error(&error);
                State::complete(static_cast<State::Job *>(data), error, file);
            }, job);
            return id;
        }
    }
    m_state->jobs.remove(id);
    delete job;
    QMetaObject::invokeMethod(this, [this, id] {
        emit finished(id, {}, tr("Место недоступно. Для сетевых папок и телефонов нужен GVfs."));
    }, Qt::QueuedConnection);
    return id;
}

int Mounts::release(const QString &location, bool eject)
{
    m_state->init();
    auto *job = m_state->start();
    const int id = job->id;
    const QByteArray uri = location.startsWith(QLatin1Char('/')) ? QUrl::fromLocalFile(location).toEncoded() : location.toUtf8();
    Ref<GFile> target(g_file_new_for_uri(uri.constData()));
    auto *mounts = g_volume_monitor_get_mounts(m_state->monitor.get());
    bool started = false;
    for (auto *entry = mounts; entry; entry = entry->next) {
        auto *mount = G_MOUNT(entry->data);
        Ref<GFile> root(g_mount_get_root(mount));
        if (!g_file_equal(root.get(), target.get())) continue;
        auto *op = job->operation.get();
        auto *cancel = job->cancellable.get();
        Ref<GDrive> drive(g_mount_get_drive(mount));
        if (eject && drive && g_drive_can_stop(drive.get())) {
            g_drive_stop(drive.get(), G_MOUNT_UNMOUNT_NONE, op, cancel, +[](GObject *obj, GAsyncResult *r, gpointer data) {
                GError *error = nullptr;
                g_drive_stop_finish(G_DRIVE(obj), r, &error);
                State::complete(static_cast<State::Job *>(data), error);
            }, job);
        } else if (eject && g_mount_can_eject(mount)) {
            g_mount_eject_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, cancel, +[](GObject *obj, GAsyncResult *r, gpointer data) {
                GError *error = nullptr;
                g_mount_eject_with_operation_finish(G_MOUNT(obj), r, &error);
                State::complete(static_cast<State::Job *>(data), error);
            }, job);
        } else if (g_mount_can_unmount(mount)) {
            g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, cancel, +[](GObject *obj, GAsyncResult *r, gpointer data) {
                GError *error = nullptr;
                g_mount_unmount_with_operation_finish(G_MOUNT(obj), r, &error);
                State::complete(static_cast<State::Job *>(data), error);
            }, job);
        } else break;
        started = true;
        break;
    }
    g_list_free_full(mounts, g_object_unref);
    if (!started) {
        m_state->jobs.remove(id);
        delete job;
        QMetaObject::invokeMethod(this, [this, id] { emit finished(id, {}, tr("Не удалось отключить устройство")); }, Qt::QueuedConnection);
    }
    return id;
}

void Mounts::reply(int id, const QVariantMap &answer)
{
    auto *job = m_state->jobs.value(id);
    if (!job || !job->awaiting) return;
    job->awaiting = false;
    auto *op = job->operation.get();
    g_mount_operation_set_username(op, answer.value("user").toString().toUtf8().constData());
    g_mount_operation_set_domain(op, answer.value("domain").toString().toUtf8().constData());
    g_mount_operation_set_password(op, answer.value("password").toString().toUtf8().constData());
    g_mount_operation_set_password_save(op, G_PASSWORD_SAVE_NEVER);
    g_mount_operation_set_anonymous(op, answer.value("anonymous").toBool());
    g_mount_operation_set_choice(op, answer.value("choice").toInt());
    g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
}

void Mounts::cancel(int id)
{
    if (auto *job = m_state->jobs.value(id)) {
        g_cancellable_cancel(job->cancellable.get());
        if (job->awaiting) {
            job->awaiting = false;
            g_mount_operation_reply(job->operation.get(), G_MOUNT_OPERATION_ABORTED);
        }
    }
}
