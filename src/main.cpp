#include <gio/gio.h>

#include "DirModel.h"
#include "Environment.h"
#include "FileManagerService.h"
#include "GlassIcons.h"
#include "IconProvider.h"
#include "Language.h"
#include "Mounts.h"
#include "Picker.h"
#include "UserPath.h"

#include <QDir>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {
const QString applicationId = QStringLiteral("io.github.nook_apps.Shelf");
const QString applicationPath = QStringLiteral("/io/github/nook_apps/Shelf");
// Тема иконок та же, что у GTK-приложений
QString gtkIconTheme()
{
    auto *source = g_settings_schema_source_get_default();
    if (!source) return {};
    auto *schema = g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", true);
    if (!schema) return {};
    auto *settings = g_settings_new_full(schema, nullptr, nullptr);
    auto *value = g_settings_get_string(settings, "icon-theme");
    const QString name = QString::fromUtf8(value);
    g_free(value);
    g_object_unref(settings);
    g_settings_schema_unref(schema);
    return name;
}

qint64 residentKiB()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) {
        for (QByteArray line = status.readLine(); !line.isEmpty(); line = status.readLine()) {
            if (line.startsWith("VmRSS:"))
                return line.simplified().split(' ').value(1).toLongLong();
        }
    }
    return 0;
}

QPair<QString, QString> destination(const QString &arg, bool select)
{
    if (Mounts::remoteUrl(arg)) return {arg, {}};
    const QString path = DirModel::expandPath(arg);
    const QFileInfo info(path);
    if (info.isDir() && !select) return {path, {}};
    const QString parent = info.absolutePath();
    return QFileInfo(parent).isDir() ? qMakePair(parent, path) : qMakePair(QDir::homePath(), QString());
}
}

int main(int argc, char *argv[])
{
    // До QGuiApplication: ему нужен экран, а --version должен работать и по SSH, и в скриптах
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--version") == 0) {
            QTextStream(stdout) << "Shelf " << SHELF_VERSION << '\n';
            return 0;
        }

    int benchArgument = 0;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--bench") == 0) { benchArgument = i; break; }
    }
    const bool benchmarking = benchArgument != 0;
    std::unique_ptr<QTemporaryDir> benchHome;
    QString benchPath;
    if (benchmarking) {
        if (argc <= benchArgument + 1 || !QFileInfo(QFile::decodeName(argv[benchArgument + 1]) + "/1000").isDir()) {
            QTextStream(stderr) << "Usage: shelf --bench FIXTURE_DIRECTORY (see tools/bench.py)\n";
            return 2;
        }
        benchPath = QFileInfo(QFile::decodeName(argv[benchArgument + 1])).absoluteFilePath();
        benchHome = std::make_unique<QTemporaryDir>();
        if (!benchHome->isValid())
            return 2;
        for (const char *name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
                                  "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
            const QString path = benchHome->filePath(QString::fromLatin1(name));
            if (!QDir().mkpath(path) || !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
                return 2;
            qputenv(name, QFile::encodeName(path));
        }
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + QFile::encodeName(benchHome->filePath("bus")));
        qputenv("GIO_USE_VFS", "local");
        qputenv("GIO_USE_VOLUME_MONITOR", "unix");
        const QString state = benchHome->filePath(QStringLiteral("XDG_STATE_HOME/shelf"));
        if (!QDir().mkpath(state))
            return 2;
        QFile views(state + QStringLiteral("/folder-views.json"));
        if (!views.open(QIODevice::WriteOnly))
            return 2;
        QJsonObject entries;
        for (int i = 0; i < 512; ++i)
            entries.insert(benchPath + QStringLiteral("/views/") + QString::number(i),
                           QJsonObject{{"sortKey", "name"}, {"gridMode", true}, {"iconSize", 56}, {"sortDesc", false}, {"saved", i}});
        const auto json = QJsonDocument(entries).toJson(QJsonDocument::Compact);
        if (views.write(json) != json.size())
            return 2;
        const QDir sourceThumbs(benchPath + QStringLiteral("/thumbnails"));
        const auto cached = benchHome->filePath(QStringLiteral("XDG_CACHE_HOME/thumbnails/normal"));
        if (!QDir().mkpath(cached)) return 2;
        for (const auto &thumb : sourceThumbs.entryList({QStringLiteral("*.png")}, QDir::Files))
            if (!QFile::copy(sourceThumbs.filePath(thumb), QDir(cached).filePath(thumb))) return 2;
    }
    QElapsedTimer startup;
    startup.start();

    // shelf --pick запрос.json — окно выбора файлов для xdg-desktop-portal-shelf
    QString pickRequest;
    for (int i = 1; !benchmarking && i + 1 < argc; ++i)
        if (qstrcmp(argv[i], "--pick") == 0)
            pickRequest = QFile::decodeName(argv[i + 1]);

    // До всего остального: по PATH ищутся matugen, терминалы, tumbler и программы для файлов
    extendUserPath();
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("shelf"));
    app.setOrganizationName(QStringLiteral("shelf"));
    app.setApplicationDisplayName(QStringLiteral("Shelf"));
    app.setApplicationVersion(QStringLiteral(SHELF_VERSION));
    const auto args = app.arguments();
    const auto action = args.value(1);
    const bool serviceStart = action == QLatin1String("--service");
    const bool selectArgument = action == QLatin1String("--select");
    const bool quitting = action == QLatin1String("--quit");
    const QString positional = args.value(selectArgument ? 2 : 1);
    const bool singleInstance = !benchmarking && pickRequest.isEmpty();
    auto bus = QDBusConnection::sessionBus();
    const bool primary = singleInstance && !quitting && bus.registerService(applicationId);
    if (singleInstance && bus.isConnected() && !primary) {
        QDBusInterface running(applicationId, applicationPath, QStringLiteral("org.freedesktop.Application"), bus);
        if (quitting) {
            if (running.isValid()) running.call(QStringLiteral("ActivateAction"), QStringLiteral("quit"), QVariantList{}, QVariantMap{});
            return 0;
        }
        if (serviceStart) return 0;
        const QString uri = Mounts::remoteUrl(positional) ? positional : QUrl::fromLocalFile(DirModel::expandPath(positional)).toString();
        const auto reply = positional.isEmpty() ? running.call(QStringLiteral("Activate"), QVariantMap{})
            : running.call(QStringLiteral("Open"), QStringList{uri},
                           QVariantMap{{QStringLiteral("select"), selectArgument}});
        if (reply.type() != QDBusMessage::ErrorMessage) return 0;
        QTextStream(stderr) << reply.errorMessage() << '\n';
        return 1;
    }
    if (quitting) return 0;
    FileManagerService fileManager;
    if (primary) {
        bus.registerObject(applicationPath, &fileManager);
        bus.registerObject(QStringLiteral("/org/freedesktop/FileManager1"), &fileManager);
        bus.registerService(QStringLiteral("org.freedesktop.FileManager1"));
        app.setQuitOnLastWindowClosed(false);
    }
    // Язык — до QML: первые же строки окна должны быть уже переведены
    Language::init();

    // Становится app_id окна в Wayland — по нему Hyprland и .desktop узнают приложение
    app.setDesktopFileName(applicationId);

    const QString iconTheme = gtkIconTheme();
    if (!iconTheme.isEmpty())
        QIcon::setThemeName(iconTheme);
    QIcon::setFallbackThemeName(QStringLiteral("Adwaita"));
    // Своя иконка из hicolor; до установки — та, что в теме
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.nook_apps.Shelf"),
                                       QIcon::fromTheme(QStringLiteral("system-file-manager"))));

    // Значки интерфейса — вшитые символы Nerd Font: не зависим от установленных шрифтов
    QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/Shelf/fonts/ShelfIcons.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/Shelf/fonts/ShelfIconsPropo.ttf"));

    // qt6ct кладёт в шрифт приложения имя начертания («Regular»), его наследуют
    // все шрифты, и оно перебивает жирность: Font.DemiBold и font.bold рисовались
    // бы обычным. Начертание выбираем по весу сами. Вес тоже обычный: у Shelf свои
    // шрифты (Theme.fontFamily), а вес из qt6ct (там Medium у Fira Code) задан для
    // другого шрифта и без «Regular» сделал бы весь текст жирнее
    QFont font = QGuiApplication::font();
    if (!font.styleName().isEmpty() || font.weight() != QFont::Normal) {
        font.setStyleName(QString());
        font.setWeight(QFont::Normal);
        QGuiApplication::setFont(font);
    }

    // Переезд на io.github.nook_apps.Shelf: старые назначения и правила — на новое имя
    if (pickRequest.isEmpty())
        Environment::migrateAppId();

    // Свои контролы рисуем сами; Basic не тянет вид qt6ct/kvantum
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    if (!pickRequest.isEmpty()) {
        if (!Picker::load(pickRequest))
            return 2;
        // Свой app_id — чтобы Hyprland держал окно выбора плавающим, по центру
        app.setDesktopFileName(QStringLiteral("io.github.nook_apps.Shelf.Picker"));
    }

    // shelf [папка | файл | file://…] — для файла открываем его папку и выделяем его
    const QString start = benchmarking ? benchPath + QStringLiteral("/1000")
        : !pickRequest.isEmpty() ? Picker().currentFolder()
        : positional.isEmpty() || serviceStart ? QDir::homePath() : positional;

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    engine.addImageProvider(QStringLiteral("glass"), new GlassIcons);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    QList<QQuickWindow *> windows;
    QPointer<QQuickWindow> lastActive;
    QObject windowLifetime;
    auto newWindow = [&](QString path, const QString &action) -> QQuickWindow * {
        if (path.isEmpty()) path = QDir::homePath();
        const auto [folder, selected] = destination(path, action == QLatin1String("select"));
        engine.setInitialProperties({{QStringLiteral("startPath"), folder}, {QStringLiteral("startSelect"), selected},
            {QStringLiteral("benchPath"), benchPath}, {QStringLiteral("externalProperties"), action == QLatin1String("properties")}});
        const auto oldCount = engine.rootObjects().size();
        engine.loadFromModule("Shelf", "Main");
        if (engine.rootObjects().size() <= oldCount) return nullptr;
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().last());
        if (!window) return nullptr;
        if (benchmarking) return window;
        windows << window;
        lastActive = window;
        QObject::connect(window, SIGNAL(newWindowRequested(QString)), &fileManager, SLOT(openWindow(QString)));
        QObject::connect(window, SIGNAL(quitShelfRequested()), &fileManager, SLOT(quit()));
        QObject::connect(window, &QQuickWindow::activeChanged, &windowLifetime, [&, window] { if (window->isActive()) lastActive = window; });
        QObject::connect(window, &QQuickWindow::visibleChanged, &windowLifetime, [&, window](bool visible) {
            if (visible) return;
            windows.removeAll(window);
            if (lastActive == window) lastActive = windows.isEmpty() ? nullptr : windows.last();
            const bool resident = primary && window->property("residentEnabled").toBool();
            window->deleteLater();
            if (windows.isEmpty()) {
                if (!resident) app.quit();
                else QTimer::singleShot(500, &windowLifetime, [&] {
                    if (windows.isEmpty()) {
                        if (auto *settings = engine.singletonInstance<QObject *>("Shelf", "ProcessSettings"))
                            settings->setProperty("idleMemoryKiB", residentKiB());
                    }
                });
            }
        });
        return window;
    };
    QObject::connect(&fileManager, &FileManagerService::quitRequested, &windowLifetime, [&] {
        const auto open = windows;
        for (auto *window : open) window->close();
        app.quit();
    });
    QObject::connect(&fileManager, &FileManagerService::openRequested, &windowLifetime,
                     [&](const QStringList &paths, const QString &action) {
        if (action == QLatin1String("activate") && lastActive) {
            lastActive->show();
            lastActive->requestActivate();
            return;
        }
        const bool separate = action == QLatin1String("window") ||
            (lastActive && lastActive->property("externalNewWindow").toBool());
        for (const auto &path : paths.isEmpty() ? QStringList{QDir::homePath()} : paths) {
            if (!lastActive || separate) { newWindow(path, action); continue; }
            const auto [folder, selected] = destination(path, action == QLatin1String("select"));
            QMetaObject::invokeMethod(lastActive, "openExternal", Q_ARG(QString, folder), Q_ARG(QString, selected),
                                      Q_ARG(bool, action == QLatin1String("properties")));
            lastActive->requestActivate();
        }
    });
    const auto beforeQml = startup.nsecsElapsed();
    const auto initialRss = benchmarking ? residentKiB() : 0;
    if (!serviceStart || !primary) newWindow(start, selectArgument ? QStringLiteral("select") : QString());
    const double qmlMs = (startup.nsecsElapsed() - beforeQml) / 1e6;
    double firstFilesFrameMs = 0;
    if (benchmarking && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&, window] {
            if (!firstFilesFrameMs && window->property("benchHasFiles").toBool())
                firstFilesFrameMs = startup.nsecsElapsed() / 1e6;
        }, Qt::QueuedConnection);
    }
    const int result = app.exec();
    if (benchmarking && result == 0) {
        if (engine.rootObjects().isEmpty()) return 2;
        const auto values = engine.rootObjects().first()->property("benchResults").toMap();
        if (values.isEmpty())
            return 2;
        auto report = QJsonObject::fromVariantMap(values);
        report.insert(QStringLiteral("qml_ms"), qmlMs);
        report.insert(QStringLiteral("first_files_frame_ms"), firstFilesFrameMs);
        report.insert(QStringLiteral("rss_growth_kib"), residentKiB() - initialRss);
        report.insert(QStringLiteral("version"), QStringLiteral(SHELF_VERSION));
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    }
    return result;
}
