// SPDX-License-Identifier: GPL-3.0-or-later
// Pure helpers that describe compiled YANG schema nodes (no GUI code).
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct lysc_node;
struct lys_module;

namespace ys {

enum class Access {
    ReadWrite, ///< config true
    ReadOnly,  ///< config false / RPC output / notification content
    Input,     ///< RPC/action input parameter
    Exec,      ///< rpc or action
    Notif,     ///< notification
    None       ///< input/output containers themselves
};

/// Iterate the children shown in the tree for a node: data children,
/// then actions, then notifications (like pyang -f tree).
QVector<const lysc_node *> childrenOf(const lysc_node *node);
/// Top-level entries of an implemented module: data, rpcs, notifications.
QVector<const lysc_node *> topLevelOf(const lys_module *mod);

QString nodeKind(const lysc_node *node);          // "container", "list", ...
QString typeName(const lysc_node *node);          // typedef name as written, or base type
QString baseTypeName(const lysc_node *node);      // built-in base type
Access access(const lysc_node *node);
QString accessShort(Access a);                    // pyang-style "rw", "ro", "-w", "-x", "-n"
QString flagsShort(const lysc_node *node);        // "*" list, "?" presence/optional, "!" etc.
QStringList listKeys(const lysc_node *node);
bool isMandatory(const lysc_node *node);
bool isDeprecated(const lysc_node *node);
bool isObsolete(const lysc_node *node);

// Paths ---------------------------------------------------------------------
QString schemaPath(const lysc_node *node);        // includes choice/case
QString dataPath(const lysc_node *node);          // XPath without keys
QString dataPathWithKeys(const lysc_node *node);  // /m:a/b[k='*']/c
/// gNMI path; with @p originPrefix the first element carries the module
/// name (rfc7951 style), otherwise plain OpenConfig style.
QString gnmiPath(const lysc_node *node, bool originPrefix);
QString restconfPath(const lysc_node *node);      // /restconf/data/m:a/b={k}/c
/// NETCONF subtree filter selecting this node.
QString subtreeFilter(const lysc_node *node);

// Rendering -----------------------------------------------------------------
QString nodeDetailsHtml(const lysc_node *node);
QString moduleDetailsHtml(const lys_module *mod);

/// pyang-like tree text for a module (libyang tree printer).
QString moduleTreeText(const lys_module *mod, int lineLength = 0);
/// Module source: the original file if available, else libyang's YANG printer.
QString moduleSource(const lys_module *mod);

} // namespace ys
