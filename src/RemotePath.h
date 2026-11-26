#pragma once

#include <QStandardPaths>
#include <QString>

inline bool isRemotePath(const QString &path)
{
    static const auto root = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/gvfs/");
    return path.startsWith(root);
}
