#pragma once

#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <memory>

class Preview : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QUrl image READ image NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)

public:
    explicit Preview(QObject *parent = nullptr);
    Q_INVOKABLE static QString kindFor(const QString &path, const QString &mimeType, bool directory);
    Q_INVOKABLE void open(const QString &path, const QString &kind);
    Q_INVOKABLE void cancel();

    QString status() const { return m_status; }
    QUrl image() const { return m_image; }
    QString text() const { return m_text; }
    bool truncated() const { return m_truncated; }

signals:
    void changed();

private:
    void launch();
    void run(const QString &program, const QStringList &args, int timeout, bool keepOutput = false);
    void finished(bool ok);
    void renderPdf(const QString &path);
    void setStatus(const QString &status);

    QProcess m_process;
    QTimer m_timeout;
    std::unique_ptr<QTemporaryDir> m_temp;
    QString m_path;
    QString m_kind;
    QString m_stage;
    QString m_status;
    QString m_text;
    QUrl m_image;
    bool m_truncated = false;
    bool m_superseded = false;
    bool m_timedOut = false;
};
