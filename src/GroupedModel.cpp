#include "GroupedModel.h"
#include "DirModel.h"

void GroupedModel::setColumns(int columns)
{
    columns = qMax(1, columns);
    if (m_columns == columns) return;
    m_columns = columns;
    rebuild();
    emit columnsChanged();
}
void GroupedModel::setRowSpan(int rows)
{
    rows = qMax(1, rows);
    if (m_rowSpan == rows) return;
    m_rowSpan = rows;
    rebuild();
    emit rowSpanChanged();
}
void GroupedModel::setSourceModel(QAbstractItemModel *source)
{
    if (source == sourceModel()) { if (!source) rebuild(); return; }
    for (const auto &c : std::as_const(m_connections)) disconnect(c);
    m_connections.clear();
    QAbstractProxyModel::setSourceModel(source);
    if (source) {
        m_connections << connect(source, &QAbstractItemModel::modelReset, this, &GroupedModel::rebuild)
                      << connect(source, &QAbstractItemModel::rowsInserted, this, &GroupedModel::rebuild)
                      << connect(source, &QAbstractItemModel::rowsRemoved, this, &GroupedModel::rebuild)
                      << connect(source, &QAbstractItemModel::dataChanged, this, &GroupedModel::rebuild)
                      << connect(source, &QAbstractItemModel::layoutChanged, this, &GroupedModel::rebuild)
                      << connect(source, &QObject::destroyed, this, [this] { setSourceModel(nullptr); });
    }
    rebuild();
}
void GroupedModel::rebuild()
{
    beginResetModel();
    m_slots.clear(); m_visual.clear();
    if (sourceModel()) {
        m_visual.resize(sourceModel()->rowCount());
        int previous = -1;
        int inRow = 0;
        QVector<int> rowFiles;
        rowFiles.reserve(m_columns);
        auto finishRow = [&] {
            if (inRow == 0) return;
            while (rowFiles.size() < m_columns) rowFiles.append(-1);
            while (m_slots.size() % m_columns) m_slots.append(Slot{});
            for (int r = 1; r < m_rowSpan; ++r)
                for (int file : std::as_const(rowFiles)) m_slots.append(Slot{-1, file, {}});
            inRow = 0;
            rowFiles.clear();
        };
        for (int i = 0; i < sourceModel()->rowCount(); ++i) {
            const int group = sourceModel()->data(sourceModel()->index(i, 0), DirModel::GroupIdRole).toInt();
            if (i == 0 || group != previous) {
                finishRow();
                const auto label = sourceModel()->data(sourceModel()->index(i, 0), DirModel::GroupLabelRole).toString();
                m_slots.append(Slot{-1, -1, label});
                for (int c = 1; c < m_columns; ++c) m_slots.append(Slot{});
                previous = group;
            }
            m_visual[i] = int(m_slots.size());
            m_slots.append(Slot{i, i, {}});
            rowFiles.append(i);
            if (++inRow == m_columns) finishRow();
        }
        finishRow();
    }
    endResetModel();
    ++m_revision;
    emit revisionChanged();
}
int GroupedModel::fileIndexAt(int i) const { return i >= 0 && i < m_slots.size() ? m_slots[i].file : -1; }
int GroupedModel::coveredFileAt(int i) const { return i >= 0 && i < m_slots.size() ? m_slots[i].covered : -1; }
QModelIndex GroupedModel::index(int row, int column, const QModelIndex &parent) const
{
    return !parent.isValid() && column == 0 && row >= 0 && row < m_slots.size() ? createIndex(row, column) : QModelIndex();
}
QModelIndex GroupedModel::mapToSource(const QModelIndex &i) const
{
    const int file = i.isValid() ? fileIndexAt(i.row()) : -1;
    return sourceModel() && file >= 0 ? sourceModel()->index(file, 0) : QModelIndex();
}
QModelIndex GroupedModel::mapFromSource(const QModelIndex &i) const
{
    return i.isValid() && i.model() == sourceModel() ? index(visualIndex(i.row()), 0) : QModelIndex();
}
QHash<int, QByteArray> GroupedModel::roleNames() const { return DirModel::fileRoleNames(); }
QVariant GroupedModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() >= m_slots.size()) return {};
    if (role < Qt::UserRole) return mapToSource(i).isValid() ? sourceModel()->data(mapToSource(i), role) : QVariant();
    const auto &slot = m_slots[i.row()];
    if (role == DirModel::FileIndexRole) return slot.file;
    if (role == DirModel::HeaderRole) return slot.file < 0 && !slot.label.isEmpty();
    if (role == DirModel::PaddingRole) return slot.file < 0 && slot.label.isEmpty();
    if (role == DirModel::GroupLabelRole) return slot.label;
    if (slot.file >= 0) return sourceModel()->data(mapToSource(i), role);
    switch (role) {
    case DirModel::IsDirRole: case DirModel::IsHiddenRole: case DirModel::IsLinkRole: case DirModel::IsLockedRole: return false;
    case DirModel::MtimeRole: case DirModel::SizeRole: return 0;
    default: return QString();
    }
}
