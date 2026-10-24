// SPDX-License-Identifier: GPL-3.0-or-later
// gNMI data <-> libyang: resolve gNMI paths against the loaded schemas, build
// data trees from notifications, and validate Set requests locally.
#pragma once

#include "core/EditConfig.h"
#include "core/GnmiTypes.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct ly_ctx;
struct lyd_node;
struct lysc_node;

namespace ys {
class DataTree;
}

namespace ys::gnmi {

struct Resolved {
    QVector<const lysc_node *> chain;  ///< one schema node per path element
    QString lyPath;                    ///< libyang data path, empty if keys are wildcards
    QString error;
    const lysc_node *node() const { return chain.isEmpty() ? nullptr : chain.last(); }
    bool ok() const { return error.isEmpty() && !chain.isEmpty(); }
};

/// Match a gNMI path against the loaded schemas. Element names may carry a
/// "module:" prefix (RFC 7951 style) or not (OpenConfig style).
Resolved resolve(const ly_ctx *ctx, const Path &path);

/// RFC 7951 JSON document holding @p value at @p path (ancestors and list
/// keys added). Scalars are typed by the schema leaf.
bool wrapJson(const ly_ctx *ctx, const Path &path, const Value &value, QByteArray *doc,
              QString *error);

/// Merge every update of @p notifications into @p tree. Deletes are listed in
/// @p messages. Returns the number of updates that could be placed.
int notificationsToTree(const ly_ctx *ctx, const QVector<Notification> &notifications,
                        DataTree *tree, QStringList *messages);

/// gNMI path of a data node, with its key values.
Path pathForDataNode(const lyd_node *node, bool withModule);
/// The node's configuration as JSON_IETF text (state left out), the value for
/// a gNMI update/replace at pathForDataNode(). Pretty-printed.
QByteArray entryJson(const lyd_node *node);

// --- Set requests --------------------------------------------------------------

/// Parse the Set editor text:
///   update  <path> = <json>
///   replace <path> = <json>
///   delete  <path>
/// A statement may continue on the following lines; '#' starts a comment
/// line. Bare scalars are accepted as JSON strings.
bool parseSetScript(const QString &text, QVector<SetOperation> *ops, QStringList *errors);
QString formatSetOperation(const SetOperation &op);

/// Validate a Set: values against the schemas, and with @p current (the
/// device's configuration) the resulting configuration and the changes.
/// Operations are applied in gNMI order: deletes, replaces, updates.
EditValidation validateSet(const ly_ctx *ctx, const QVector<SetOperation> &ops,
                           const DataTree *current);

} // namespace ys::gnmi
