// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/DataTree.h"

#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <cstdlib>

namespace ys {

namespace {
// LYD_PRINT_WITHSIBLINGS was renamed LYD_PRINT_SIBLINGS; the value is 0x01 in both.
constexpr uint32_t kPrintSiblings = 0x01;

QString takeString(char *s)
{
    QString out = s ? QString::fromUtf8(s) : QString();
    std::free(s);
    return out;
}

const char *moduleNameOf(const lyd_node *n)
{
    if (n->schema)
        return n->schema->module->name;
    return nullptr;
}
} // namespace

DataTree::~DataTree()
{
    clear();
}

DataTree::DataTree(DataTree &&o) noexcept : m_root(o.m_root), m_messages(std::move(o.m_messages))
{
    o.m_root = nullptr;
}

DataTree &DataTree::operator=(DataTree &&o) noexcept
{
    if (this != &o) {
        clear();
        m_root = o.m_root;
        m_messages = std::move(o.m_messages);
        o.m_root = nullptr;
    }
    return *this;
}

void DataTree::clear()
{
    if (m_root)
        lyd_free_all(m_root);
    m_root = nullptr;
}

bool DataTree::parseXml(const ly_ctx *ctx, const QByteArray &xml)
{
    clear();
    m_messages.clear();
    if (!ctx)
        return false;
    if (xml.trimmed().isEmpty())
        return true; // empty data is valid
    LyLogCapture cap(&m_messages);
    const LY_ERR err = lyd_parse_data_mem(ctx, xml.constData(), LYD_XML,
                                          LYD_PARSE_ONLY | LYD_PARSE_OPAQ, 0, &m_root);
    if (err != LY_SUCCESS) {
        clear();
        if (m_messages.isEmpty())
            m_messages << QStringLiteral("error: could not parse data (libyang error %1)").arg(int(err));
        return false;
    }
    return true;
}

QString DataTree::toJson() const
{
    if (!m_root)
        return {};
    char *s = nullptr;
    lyd_print_mem(&s, m_root, LYD_JSON, kPrintSiblings);
    return takeString(s);
}

QString DataTree::toXml() const
{
    if (!m_root)
        return {};
    char *s = nullptr;
    lyd_print_mem(&s, m_root, LYD_XML, kPrintSiblings);
    return takeString(s);
}

const lyd_node *DataTree::firstChild(const lyd_node *n)
{
    return n ? lyd_child(n) : nullptr;
}

const lyd_node *DataTree::next(const lyd_node *n)
{
    return n ? n->next : nullptr;
}

bool DataTree::isOpaque(const lyd_node *n)
{
    return n && !n->schema;
}

const lysc_node *DataTree::schema(const lyd_node *n)
{
    return n ? n->schema : nullptr;
}

QString DataTree::name(const lyd_node *n)
{
    if (!n)
        return {};
    if (!n->schema) {
        auto *o = reinterpret_cast<const lyd_node_opaq *>(n);
        QString s = QString::fromUtf8(o->name.name);
        if (o->name.prefix)
            s = QString::fromUtf8(o->name.prefix) + QLatin1Char(':') + s;
        return s;
    }
    const lyd_node *parent = lyd_parent(n);
    const char *mod = moduleNameOf(n);
    const char *pmod = parent ? moduleNameOf(parent) : nullptr;
    QString s = QString::fromUtf8(n->schema->name);
    if (!parent || !pmod || qstrcmp(mod, pmod) != 0)
        s = QString::fromUtf8(mod) + QLatin1Char(':') + s;
    return s;
}

QString DataTree::value(const lyd_node *n)
{
    if (!n)
        return {};
    if (!n->schema) {
        auto *o = reinterpret_cast<const lyd_node_opaq *>(n);
        return lyd_child(n) ? QString() : QString::fromUtf8(o->value).trimmed();
    }
    if (n->schema->nodetype & LYD_NODE_TERM)
        return QString::fromUtf8(lyd_get_value(n));
    return {};
}

QString DataTree::path(const lyd_node *n)
{
    return n ? takeString(lyd_path(n, LYD_PATH_STD, nullptr, 0)) : QString();
}

int DataTree::countNodes(const lyd_node *first)
{
    int c = 0;
    for (const lyd_node *n = first; n; n = n->next)
        c += 1 + countNodes(lyd_child(n));
    return c;
}

} // namespace ys
