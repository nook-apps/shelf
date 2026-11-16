#pragma once
#include <QAbstractProxyModel>
#include <QVector>
#include <QtQml/qqmlregistration.h>

// Виртуальные ячейки GridView: заголовок занимает строку, файлы остаются в DirModel.
class GroupedModel : public QAbstractProxyModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged)
public:
    explicit GroupedModel(QObject *parent = nullptr) : QAbstractProxyModel(parent) {}
    int revision() const { return m_revision; }
    int columns() const { return m_columns; }
    void setColumns(int columns);
    void setSourceModel(QAbstractItemModel *source) override;
    QModelIndex mapToSource(const QModelIndex &index) const override;
    QModelIndex mapFromSource(const QModelIndex &index) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &) const override { return {}; }
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_slots.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 1; }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE int visualIndex(int fileIndex) const { return m_visual.value(fileIndex, -1); }
    Q_INVOKABLE int fileIndexAt(int visualIndex) const;
signals:
    void columnsChanged();
    void revisionChanged();
private:
    void rebuild();
    struct Slot { int file = -1; QString label; };
    int m_revision = 0;
    int m_columns = 1;
    QVector<Slot> m_slots;
    QVector<int> m_visual;
    QList<QMetaObject::Connection> m_connections;
};
