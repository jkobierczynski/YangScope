// SPDX-License-Identifier: GPL-3.0-or-later
// gNMI paths in their string form (gNMI path conventions):
//   [origin:]/elem/elem[key=value][key2=value2]/elem
// Values may contain ']' or '/' escaped with a backslash.
#pragma once

#include <QPair>
#include <QString>
#include <QVector>

struct lysc_node;

namespace ys::gnmi {

struct PathElem {
    QString name;
    QVector<QPair<QString, QString>> keys;  ///< in the order given
    bool operator==(const PathElem &o) const { return name == o.name && keys == o.keys; }
};

struct Path {
    QString origin;
    QString target;
    QVector<PathElem> elems;
    bool operator==(const Path &o) const
    {
        return origin == o.origin && target == o.target && elems == o.elems;
    }
    bool isRoot() const { return elems.isEmpty(); }
    /// prefix + this
    Path joined(const Path &suffix) const;
};

/// Parse a path string. Returns false and sets @p error on syntax errors.
bool parsePath(const QString &text, Path *out, QString *error = nullptr);
QString pathToString(const Path &p);

/// gNMI path for a schema node. With @p withModule the first element (and any
/// element whose module differs from its parent) is written "module:name".
/// List keys are added as "*" wildcards.
Path pathForSchemaNode(const lysc_node *node, bool withModule);

} // namespace ys::gnmi
