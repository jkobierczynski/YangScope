// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/DataTreeWidget.h"

#include "core/DataTree.h"
#include "core/NodeInfo.h"

#include <libyang/libyang.h>

#include <QHeaderView>
#include <QMenu>

namespace ys {

namespace {
constexpr int kSchemaRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kAutoExpandDepth = 3;
constexpr int kMaxRows = 200000;

/// "interface [name=eth0]" for list entries.
QString listLabel(const lyd_node *n)
{
    QString s = DataTree::name(n);
    const lysc_node *sch = DataTree::schema(n);
    if (!sch || sch->nodetype != LYS_LIST)
        return s;
    QStringList keys;
    for (const lyd_node *c = DataTree::firstChild(n); c; c = DataTree::next(c)) {
        const lysc_node *cs = DataTree::schema(c);
        if (cs && (cs->flags & LYS_KEY))
            keys << QString::fromUtf8(cs->name) + QLatin1Char('=') + DataTree::value(c);
    }
    if (!keys.isEmpty())
        s += QStringLiteral(" [") + keys.join(QStringLiteral(", ")) + QLatin1Char(']');
    return s;
}
} // namespace

DataTreeWidget::DataTreeWidget(QWidget *parent) : QTreeWidget(parent)
{
    setColumnCount(3);
    setHeaderLabels({tr("Node"), tr("Value"), tr("Type")});
    setUniformRowHeights(true);
    setAlternatingRowColors(true);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Interactive);
    header()->setSectionResizeMode(1, QHeaderView::Stretch);
    header()->setSectionResizeMode(2, QHeaderView::Interactive);
    setColumnWidth(0, 280);
    setColumnWidth(2, 150);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (!it)
            return;
        auto *s = static_cast<const lysc_node *>(it->data(0, kSchemaRole).value<const void *>());
        if (s)
            emit schemaNodeActivated(s);
    });
    connect(this, &QWidget::customContextMenuRequested, this, &DataTreeWidget::showContextMenu);
}

void DataTreeWidget::clearTree()
{
    clear();
    m_count = 0;
}

void DataTreeWidget::showTree(const DataTree &tree)
{
    setUpdatesEnabled(false);
    clearTree();
    addNodes(invisibleRootItem(), tree.root(), 0);
    setUpdatesEnabled(true);
}

void DataTreeWidget::addNodes(QTreeWidgetItem *parent, const lyd_node *first, int depth)
{
    for (const lyd_node *n = first; n && m_count < kMaxRows; n = DataTree::next(n)) {
        ++m_count;
        const lysc_node *sch = DataTree::schema(n);
        auto *it = new QTreeWidgetItem(parent);
        it->setText(0, listLabel(n));
        const QString v = DataTree::value(n);
        it->setText(1, v);
        it->setToolTip(1, v);
        if (sch) {
            it->setText(2, (sch->nodetype & (LYS_LEAF | LYS_LEAFLIST)) ? typeName(sch) : nodeKind(sch));
            it->setData(0, kSchemaRole, QVariant::fromValue(static_cast<const void *>(sch)));
            if (access(sch) == Access::ReadOnly)
                it->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text).darker(110));
        } else {
            QFont f = it->font(0);
            f.setItalic(true);
            it->setFont(0, f);
            it->setText(2, tr("(no schema)"));
            it->setToolTip(0, tr("This element is not described by the loaded YANG modules."));
        }
        it->setData(0, kPathRole, DataTree::path(n));
        if (const lyd_node *c = DataTree::firstChild(n)) {
            addNodes(it, c, depth + 1);
            if (depth < kAutoExpandDepth)
                it->setExpanded(true);
        }
    }
}

void DataTreeWidget::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = itemAt(pos);
    if (!it)
        return;
    QMenu menu(this);
    const QString path = it->data(0, kPathRole).toString();
    const QString value = it->text(1);
    menu.addAction(tr("Copy Value"), this, [this, value] { emit copyRequested(value, tr("value")); })
        ->setEnabled(!value.isEmpty());
    menu.addAction(tr("Copy Instance Path"), this, [this, path] { emit copyRequested(path, tr("instance path")); })
        ->setEnabled(!path.isEmpty());
    menu.addSeparator();
    menu.addAction(tr("Expand All"), this, &QTreeWidget::expandAll);
    menu.addAction(tr("Collapse All"), this, &QTreeWidget::collapseAll);
    menu.exec(viewport()->mapToGlobal(pos));
}

} // namespace ys
