// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QTreeWidget>

struct lyd_node;
struct lysc_node;

namespace ys {

class DataTree;

/// Shows instance data (a libyang data tree) with names, values and types.
/// Activating a row emits the schema node so the schema tree can follow.
class DataTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    enum class EntryAction { FilterSubtree, FilterXPath, Edit, Delete };

    explicit DataTreeWidget(QWidget *parent = nullptr);
    /// Build rows from @p tree (which must outlive the rows' use).
    void showTree(const DataTree &tree);
    void clearTree();
    int nodeCount() const { return m_count; }
    /// Label the entry actions for gNMI: FilterSubtree = "Get", FilterXPath =
    /// "Subscribe", Edit/Delete produce a Set.
    void setGnmiMode(bool on) { m_gnmi = on; }

signals:
    void schemaNodeActivated(const lysc_node *node);
    void copyRequested(const QString &text, const QString &what);
    /// Context menu action on a data node (pointer valid while the tree is shown).
    void entryAction(const lyd_node *node, ys::DataTreeWidget::EntryAction action);

private:
    void addNodes(QTreeWidgetItem *parent, const lyd_node *first, int depth);
    void showContextMenu(const QPoint &pos);
    int m_count = 0;
    bool m_gnmi = false;
};

} // namespace ys
