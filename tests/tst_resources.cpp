#include "Thumbnailer.h"
#include "ArchiveOutput.h"
#include "portal/FileChooser.h"
#include <QDBusMessage>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TestResources : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-shelf-test-bus");
    }
    void archiveOutputIsBoundedAndHandlesSplitLines()
    {
        ArchiveOutput output;
        output.append("a");
        output.append(" first\na");
        output.append(" second\nwarning\na third");
        QCOMPARE(output.added(), 3);
        for (int i = 0; i < 10000; ++i) output.append("\na file");
        QCOMPARE(output.added(), 10003);
        QVERIFY(output.tail().size() <= ArchiveOutput::tailLimit);
        output.append(QByteArray(100000, 'x') + "\nerror: disk full");
        QCOMPARE(output.tail().size(), ArchiveOutput::tailLimit);
        QVERIFY(output.tail().endsWith("error: disk full"));
    }
    void thumbnailCachesEvictOldEntries()
    {
        Thumbnailer t;
        for (int i = 0; i < 5000; ++i)
            t.m_never.insert(QStringLiteral("/nonexistent-shelf-audit/%1").arg(i), new qint64(1));
        QCOMPARE(t.m_never.size(), 4096);
        QVERIFY(!t.m_never.contains("/nonexistent-shelf-audit/0"));
        QVERIFY(t.m_never.contains("/nonexistent-shelf-audit/4999"));
        // Ready-кэш использует то же LRU-вытеснение; обращение сохраняет горячую запись.
        t.m_known.insert("hot", new Thumbnailer::Known{1, "file:///hot"});
        for (int i = 0; i < 5000; ++i) {
            QVERIFY(t.m_known.object("hot"));
            t.m_known.insert(QString::number(i), new Thumbnailer::Known{1, "file:///thumb"});
        }
        QCOMPARE(t.m_known.size(), 4096);
        QVERIFY(t.m_known.contains("hot"));
        QVERIFY(!t.m_known.contains("0"));
    }
    void thumbnailReadsCacheAsynchronouslyAndDropsOldJobs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Thumbnailer thumbnails;
        thumbnails.m_cacheDir = dir.filePath("cache");
        const QString path = dir.filePath("picture.jpg");
        QImage image(10, 10, QImage::Format_RGB32);
        image.fill(Qt::red);
        image.setText(QStringLiteral("Thumb::MTime"), QStringLiteral("42"));
        QVERIFY(QDir().mkpath(dir.filePath("cache/normal")));
        QVERIFY(image.save(dir.filePath("cache/normal/" + Thumbnailer::cacheName(path))));
        thumbnails.watch(path);
        QVERIFY(thumbnails.thumbnail(path, "image/jpeg", 48, 42).isEmpty());
        QTRY_VERIFY(!thumbnails.thumbnail(path, "image/jpeg", 48, 42).isEmpty());
        thumbnails.m_pending.insert("0:" + path, 42);
        thumbnails.m_supported.insert("image/jpeg");
        thumbnails.serviceChanged(QStringLiteral(":1.1"), {});
        QVERIFY(thumbnails.m_pending.isEmpty());
        QVERIFY(thumbnails.m_supported.isEmpty());
        QVERIFY(!thumbnails.thumbnail(path, "image/jpeg", 48, 42).isEmpty());
        thumbnails.unwatch(path);
    }
    void portalFailedStartReleasesRequest()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile exe(temp.path() + "/shelf");
        QVERIFY(exe.open(QIODevice::WriteOnly));
        exe.write("#!/nonexistent-shelf-test-interpreter\n");
        exe.close();
        QVERIFY(exe.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", temp.path().toUtf8());
        QObject root;
        FileChooser chooser(&root);
        QSignalSpy busy(&chooser, &FileChooser::busy), idle(&chooser, &FileChooser::idle);
        const auto message = QDBusMessage::createMethodCall("org.test.Shelf", "/test", "org.test.Shelf", "OpenFile");
        for (int i = 0; i < 3; ++i) {
            QVariantMap results;
            chooser.OpenFile(QDBusObjectPath(QStringLiteral("/test/request%1").arg(i)), {}, {}, {}, {}, message, results);
            QTRY_COMPARE(idle.count(), i + 1);
            QTRY_COMPARE(chooser.children().size(), 0);
        }
        QCOMPARE(busy.count(), 3);
        qputenv("PATH", oldPath);
    }
};
QTEST_GUILESS_MAIN(TestResources)
#include "tst_resources.moc"
