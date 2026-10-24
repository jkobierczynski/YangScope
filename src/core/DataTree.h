// SPDX-License-Identifier: GPL-3.0-or-later
// Instance data parsed with libyang (e.g. the <data> of a NETCONF reply).
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

struct ly_ctx;
struct lyd_node;
struct lysc_node;

namespace ys {

class DataTree {
public:
    DataTree() = default;
    ~DataTree();
    DataTree(const DataTree &) = delete;
    DataTree &operator=(const DataTree &) = delete;
    DataTree(DataTree &&o) noexcept;
    DataTree &operator=(DataTree &&o) noexcept;

    /// Parse XML instance data (siblings allowed). Elements without a schema
    /// are kept as opaque nodes rather than rejected. Values are not
    /// validated, only parsed.
    bool parseXml(const ly_ctx *ctx, const QByteArray &xml);

    const lyd_node *root() const { return m_root; }
    bool isEmpty() const { return m_root == nullptr; }
    QStringList messages() const { return m_messages; }

    QString toJson() const;   ///< RFC 7951 JSON
    QString toXml() const;

    // Node helpers (also usable for nodes of other trees) ---------------------
    static const lyd_node *firstChild(const lyd_node *n);
    static const lyd_node *next(const lyd_node *n);
    static QString name(const lyd_node *n);          ///< with module prefix if it changes
    static QString value(const lyd_node *n);         ///< canonical value of a term node
    static bool isOpaque(const lyd_node *n);
    static const lysc_node *schema(const lyd_node *n);
    static QString path(const lyd_node *n);          ///< data path with key predicates
    static int countNodes(const lyd_node *first);

private:
    void clear();
    lyd_node *m_root = nullptr;
    QStringList m_messages;
};

} // namespace ys
