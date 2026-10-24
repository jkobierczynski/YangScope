// SPDX-License-Identifier: GPL-3.0-or-later
// Local validation of NETCONF edit-config content (RFC 6241 section 7.2)
// with libyang, and XML snippets built from instance data entries.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct ly_ctx;
struct lyd_node;
struct lysc_node;

namespace ys {

class DataTree;

/// One change the edit would make, from libyang's diff.
struct EditChange {
    enum Kind { Create, Delete, Replace, Move };
    Kind kind = Create;
    QString path;      ///< instance path
    QString value;     ///< new value (leaf / leaf-list)
    QString oldValue;  ///< previous value (replace)
    QString toString() const;
};

struct EditValidation {
    bool wellFormed = false;    ///< XML parsed
    bool schemaValid = false;   ///< every element known, values valid, no state data
    bool fullValidation = false;///< result checked against the device's current config
    bool ok = false;            ///< nothing blocks sending
    QStringList errors;
    QStringList warnings;
    QVector<EditChange> changes;
    int operations = 0;         ///< explicit nc:operation attributes seen
};

enum class DefaultOperation { Merge, Replace, None };

/// Validate edit-config content (the children of <config>, which may carry
/// nc:operation attributes in the base:1.0 namespace).
///
/// With @p currentConfig (the target datastore's current content, e.g. from
/// get-config), the edit is applied to a copy following the operation
/// semantics and the result is fully validated (mandatory nodes, must/when,
/// leafrefs, unique, min/max-elements); the diff lists every change.
/// Without it only the edit itself is checked.
EditValidation validateEdit(const ly_ctx *ctx, const QByteArray &configContent,
                            const DataTree *currentConfig,
                            DefaultOperation defaultOp = DefaultOperation::Merge);

/// Changes between two configurations (e.g. running and candidate before a
/// commit). Elements without a schema are ignored and counted in warnings.
QVector<EditChange> diffConfigs(const ly_ctx *ctx, const DataTree &from, const DataTree &to,
                                QStringList *warnings = nullptr);

/// Edit-config skeleton for a schema node: its ancestors with empty key
/// elements to fill in, and the node itself.
QString schemaSkeletonXml(const lysc_node *node);

// --- XML built from instance data ------------------------------------------

enum class EntryXml {
    Config,  ///< the entry and its config descendants, with its ancestors and their keys
    Delete,  ///< ancestors + the entry's keys, marked nc:operation="delete"
    Remove,  ///< same with nc:operation="remove"
};

/// Edit-config content for @p node (children of <config>). Indented.
QString entryEditXml(const lyd_node *node, EntryXml kind);
/// <filter type="subtree"> selecting exactly this entry (keys as content
/// match nodes along the way).
QByteArray entrySubtreeFilter(const lyd_node *node);
/// XPath of the entry with key predicates, and the prefixes it uses.
QString entryXPath(const lyd_node *node, QVector<QPair<QString, QString>> *prefixes);

} // namespace ys
