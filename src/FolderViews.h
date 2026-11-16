#pragma once

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// Только явно изменённый вид; чтение папки не создаёт запись.
class FolderViews : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit FolderViews(QObject *parent = nullptr) : QObject(parent) {}
    Q_INVOKABLE QVariantMap get(const QString &path) const;
    Q_INVOKABLE bool save(const QString &path, const QVariantMap &view);
    Q_INVOKABLE bool reset(const QString &path);
    static constexpr int limit = 512;
private:
    bool update(const QString &path, const QVariantMap &view);
};
