// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers for libyang data trees shared by the NETCONF and gNMI validators.
#pragma once

#include "core/EditConfig.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct ly_ctx;
struct lyd_node;

namespace ys::lydutil {

/// Node at a libyang data path ("/mod:a/b[k='v']"), or nullptr.
lyd_node *findPath(lyd_node *tree, const QString &path);
/// Free @p node, keeping @p *first pointing at the first top-level sibling.
void freeNode(lyd_node **first, lyd_node *node);
/// Remove nodes without a schema (opaque); returns how many were removed.
int stripOpaque(lyd_node **first);
/// Turn a libyang diff tree into change lines.
void collectChanges(const ly_ctx *ctx, const lyd_node *diff, QVector<EditChange> *out);
/// Validate *result as a complete configuration and diff it against @p base.
void validateAndDiff(const ly_ctx *ctx, const lyd_node *base, lyd_node **result,
                     QStringList *errors, QVector<EditChange> *changes);

} // namespace ys::lydutil
