// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/SchemaModel.h"

#include "core/NodeInfo.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace ys {

namespace {

constexpr int kModuleIconKey = -1;

struct Badge {
    const char *letters;
    QColor color;
};

Badge badgeFor(uint16_t nodetype)
{
    switch (nodetype) {
    case LYS_CONTAINER: return {"C", QColor(0x3b, 0x82, 0xf6)};
    case LYS_LIST:      return {"L", QColor(0x8b, 0x5c, 0xf6)};
    case LYS_LEAF:      return {"l", QColor(0x10, 0xb9, 0x81)};
    case LYS_LEAFLIST:  return {"ll", QColor(0x0d, 0x94, 0x88)};
    case LYS_CHOICE:    return {"?", QColor(0xf5, 0x9e, 0x0b)};
    case LYS_CASE:      return {":", QColor(0xd9, 0x77, 0x06)};
    case LYS_RPC:       return {"R", QColor(0xef, 0x44, 0x44)};
    case LYS_ACTION:    return {"A", QColor(0xdc, 0x26, 0x26)};
    case LYS_NOTIF:     return {"N", QColor(0xec, 0x48, 0x99)};
    case LYS_INPUT:     return {"in", QColor(0x64, 0x74, 0x8b)};
    case LYS_OUTPUT:    return {"out", QColor(0x64, 0x74, 0x8b)};
    case LYS_ANYDATA:
    case LYS_ANYXML:    return {"*", QColor(0x71, 0x71, 0x7a)};
    default:            return {"", QColor(0x71, 0x71, 0x7a)};
    }
}

QIcon makeBadge(const QString &letters, const QColor &color)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int size = 16;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1, size - 1), 3.5, 3.5);
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPixelSize(letters.size() >= 3 ? 7 : letters.size() == 2 ? 8 : 10);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, letters);
    return QIcon(pm);
}

} // namespace

SchemaModel::SchemaModel(QObject *parent) : QAbstractItemModel(parent) {}
SchemaModel::~SchemaModel() = default;

void SchemaModel::clear()
{
    beginResetModel();
    m_roots.clear();
    endResetModel();
}

void SchemaModel::rebuild(const YangContext &ctx, bool includeInternal)
{
    beginResetModel();
    m_roots.clear();
    int row = 0;
    for (const lys_module *mod : ctx.implementedModules(includeInternal)) {
        auto item = std::make_unique<Item>();
        item->module = mod;
        item->row = row++;
        item->hasKids = !topLevelOf(mod).isEmpty();
        m_roots.push_back(std::move(item));
    }
    endResetModel();
}

SchemaModel::Item *SchemaModel::itemAt(const QModelIndex &index) const
{
    return index.isValid() ? static_cast<Item *>(index.internalPointer()) : nullptr;
}

void SchemaModel::populate(Item *item) const
{
    if (!item || item->populated)
        return;
    item->populated = true;
    const QVector<const lysc_node *> kids =
        item->node ? childrenOf(item->node) : topLevelOf(item->module);
    item->children.reserve(size_t(kids.size()));
    int row = 0;
    for (const lysc_node *n : kids) {
        auto child = std::make_unique<Item>();
        child->module = item->module;
        child->node = n;
        child->parent = item;
        child->row = row++;
        child->hasKids = !childrenOf(n).isEmpty();
        item->children.push_back(std::move(child));
    }
}

QModelIndex SchemaModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColCount)
        return {};
    if (!parent.isValid()) {
        if (row >= int(m_roots.size()))
            return {};
        return createIndex(row, column, m_roots[size_t(row)].get());
    }
    Item *p = itemAt(parent);
    populate(p);
    if (row >= int(p->children.size()))
        return {};
    return createIndex(row, column, p->children[size_t(row)].get());
}

QModelIndex SchemaModel::parent(const QModelIndex &child) const
{
    Item *it = itemAt(child);
    if (!it || !it->parent)
        return {};
    return createIndex(it->parent->row, 0, it->parent);
}

int SchemaModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_roots.size());
    if (parent.column() != 0)
        return 0;
    Item *p = itemAt(parent);
    if (!p->hasKids)
        return 0;
    populate(p);
    return int(p->children.size());
}

int SchemaModel::columnCount(const QModelIndex &) const
{
    return ColCount;
}

bool SchemaModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return !m_roots.empty();
    if (parent.column() != 0)
        return false;
    return itemAt(parent)->hasKids;
}

QString SchemaModel::displayName(const Item *item) const
{
    if (!item->node) {
        QString s = QString::fromUtf8(item->module->name);
        if (item->module->revision)
            s += QLatin1Char('@') + QString::fromUtf8(item->module->revision);
        return s;
    }
    const lysc_node *n = item->node;
    QString name = QString::fromUtf8(n->name);
    // Show the module prefix for nodes that come from another module (augments).
    if (item->parent && item->parent->node && item->parent->node->module != n->module)
        name = QString::fromUtf8(n->module->prefix) + QLatin1Char(':') + name;
    switch (n->nodetype) {
    case LYS_CHOICE:
        return QLatin1Char('(') + name + QLatin1Char(')') + flagsShort(n);
    case LYS_CASE:
        return QStringLiteral(":(") + name + QLatin1Char(')');
    case LYS_LIST: {
        const QStringList keys = listKeys(n);
        QString s = name + QLatin1Char('*');
        if (!keys.isEmpty())
            s += QStringLiteral(" [") + keys.join(QLatin1Char(' ')) + QLatin1Char(']');
        return s;
    }
    default:
        return name + flagsShort(n);
    }
}

QIcon SchemaModel::iconFor(const Item *item) const
{
    const int key = item->node ? int(item->node->nodetype) : kModuleIconKey;
    auto it = m_icons.constFind(key);
    if (it != m_icons.constEnd())
        return *it;
    QIcon icon = item->node
                     ? [&] { Badge b = badgeFor(item->node->nodetype);
                             return makeBadge(QString::fromLatin1(b.letters), b.color); }()
                     : makeBadge(QStringLiteral("M"), QColor(0x33, 0x41, 0x55));
    m_icons.insert(key, icon);
    return icon;
}

QVariant SchemaModel::data(const QModelIndex &index, int role) const
{
    const Item *it = itemAt(index);
    if (!it)
        return {};
    const lysc_node *n = it->node;

    if (role == NodeRole)
        return QVariant::fromValue(static_cast<const void *>(n));
    if (role == ModuleRole)
        return QVariant::fromValue(static_cast<const void *>(it->module));

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:   return displayName(it);
        case ColKind:   return n ? nodeKind(n) : QStringLiteral("module");
        case ColAccess: return n ? accessShort(access(n)) : QString();
        case ColType:   return n ? typeName(n) : QString();
        case ColModule:
            return n ? QString::fromUtf8(n->module->name)
                     : QString::fromUtf8(it->module->prefix);
        }
        return {};
    }
    if (role == Qt::DecorationRole && index.column() == ColName)
        return iconFor(it);
    if (role == Qt::ToolTipRole && index.column() == ColName && n) {
        QString tip = dataPath(n);
        if (n->dsc)
            tip += QStringLiteral("\n\n") + QString::fromUtf8(n->dsc).simplified().left(400);
        return tip;
    }
    if (role == Qt::FontRole && n && (isDeprecated(n) || isObsolete(n))) {
        QFont f;
        f.setItalic(true);
        f.setStrikeOut(isObsolete(n));
        return f;
    }
    if (role == Qt::ForegroundRole) {
        const QPalette pal = QApplication::palette();
        if (!n && !it->hasKids)
            return pal.color(QPalette::Disabled, QPalette::Text); // augment-only module
        if (n && (isObsolete(n) || index.column() == ColModule || index.column() == ColKind))
            return pal.color(QPalette::Disabled, QPalette::Text);
    }
    return {};
}

QVariant SchemaModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColName:   return tr("Name");
    case ColKind:   return tr("Kind");
    case ColAccess: return tr("Access");
    case ColType:   return tr("Type");
    case ColModule: return tr("Module");
    }
    return {};
}

const lysc_node *SchemaModel::nodeAt(const QModelIndex &index) const
{
    const Item *it = itemAt(index);
    return it ? it->node : nullptr;
}

const lys_module *SchemaModel::moduleAt(const QModelIndex &index) const
{
    const Item *it = itemAt(index);
    return it ? it->module : nullptr;
}

QModelIndex SchemaModel::indexForModule(const lys_module *mod) const
{
    for (const auto &r : m_roots)
        if (r->module == mod)
            return createIndex(r->row, 0, r.get());
    return {};
}

QModelIndex SchemaModel::indexForNode(const lysc_node *node)
{
    if (!node)
        return {};
    QVector<const lysc_node *> chain;
    for (const lysc_node *n = node; n; n = n->parent)
        chain.prepend(n);

    // The tree root for this node is the module of its top-level ancestor.
    Item *cur = nullptr;
    for (const auto &r : m_roots)
        if (r->module == chain.first()->module) {
            cur = r.get();
            break;
        }
    if (!cur)
        return {};

    for (const lysc_node *target : chain) {
        populate(cur);
        Item *next = nullptr;
        for (const auto &c : cur->children)
            if (c->node == target) {
                next = c.get();
                break;
            }
        if (!next)
            return {};
        cur = next;
    }
    return createIndex(cur->row, 0, cur);
}

} // namespace ys
