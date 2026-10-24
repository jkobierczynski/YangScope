// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>
#include <QVector>

#include <memory>

struct lys_module;
struct lysc_node;

namespace ys {

class YangContext;

/// Tree model over the compiled schema of all implemented modules.
/// Children are created lazily, so huge model sets stay responsive.
class SchemaModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { ColName, ColKind, ColAccess, ColType, ColModule, ColCount };
    enum Roles { NodeRole = Qt::UserRole + 1, ModuleRole };

    explicit SchemaModel(QObject *parent = nullptr);
    ~SchemaModel() override;

    /// Rebuild from the modules currently implemented in @p ctx.
    void rebuild(const YangContext &ctx, bool includeInternal = false);
    void clear();

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    const lysc_node *nodeAt(const QModelIndex &index) const;
    const lys_module *moduleAt(const QModelIndex &index) const;

    /// Model index of a schema node (populating parents as needed).
    QModelIndex indexForNode(const lysc_node *node);
    QModelIndex indexForModule(const lys_module *mod) const;

    int moduleCount() const { return int(m_roots.size()); }

private:
    struct Item {
        const lys_module *module = nullptr;
        const lysc_node *node = nullptr;
        Item *parent = nullptr;
        int row = 0;
        bool populated = false;
        bool hasKids = false;
        std::vector<std::unique_ptr<Item>> children;
    };

    Item *itemAt(const QModelIndex &index) const;
    void populate(Item *item) const;
    QString displayName(const Item *item) const;
    QIcon iconFor(const Item *item) const;

    std::vector<std::unique_ptr<Item>> m_roots;
    mutable QHash<int, QIcon> m_icons;
};

} // namespace ys
