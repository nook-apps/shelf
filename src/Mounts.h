#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

class Mounts : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)

public:
    explicit Mounts(QObject *parent = nullptr);
    ~Mounts() override;
    QVariantList devices() const;
    Q_INVOKABLE static bool remoteUrl(const QString &path);
    Q_INVOKABLE bool networkAvailable() const;
    Q_INVOKABLE int open(const QString &location);
    Q_INVOKABLE int release(const QString &location, bool eject);
    Q_INVOKABLE void reply(int id, const QVariantMap &answer);
    Q_INVOKABLE void cancel(int id);

signals:
    void devicesChanged();
    void prompt(int id, const QVariantMap &details);
    void finished(int id, const QString &path, const QString &error);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
