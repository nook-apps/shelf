#include "Thumbnailer.h"
#include "ArchiveOutput.h"
#include "portal/FileChooser.h"
#include <QDBusMessage>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TestResources : public QObject
{
    Q_OBJECT
private slots:
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
        t.m_supportedLoaded = true;
        for (int i = 0; i < 5000; ++i)
            t.thumbnail(QStringLiteral("/nonexistent-shelf-audit/%1").arg(i), "unsupported/type");
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
