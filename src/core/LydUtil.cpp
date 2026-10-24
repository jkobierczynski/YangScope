// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/LydUtil.h"

#include "core/DataTree.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>
#include <libyang/metadata.h>

#include <functional>

namespace ys::lydutil {

namespace {
constexpr int kMaxChanges = 2000;
}

lyd_node *findPath(lyd_node *tree, const QString &path)
{
    if (!tree || path.isEmpty())
        return nullptr;
    lyd_node *match = nullptr;
    const QByteArray p = path.toUtf8();
    if (lyd_find_path(tree, p.constData(), 0, &match) != LY_SUCCESS)
        return nullptr;
    return match;
}

/// Free @p node, keeping @p *first pointing at the first top-level sibling.
void freeNode(lyd_node **first, lyd_node *node)
{
    if (!node)
        return;
    if (node == *first)
        *first = node->next;
    lyd_free_tree(node);
}

int stripOpaque(lyd_node **first)
{
    int removed = 0;
    std::function<void(lyd_node **, lyd_node *)> walk = [&](lyd_node **head, lyd_node *n) {
        while (n) {
            lyd_node *next = n->next;
            if (!n->schema) {
                freeNode(head, n);
                ++removed;
            } else if (lyd_node *c = lyd_child(n)) {
                lyd_node *childHead = c;
                walk(&childHead, c);
            }
            n = next;
        }
    };
    walk(first, *first);
    return removed;
}

void collectChanges(const ly_ctx *ctx, const lyd_node *diff, QVector<EditChange> *out)
{
    const lys_module *yang = ly_ctx_get_module_implemented(ctx, "yang");
    if (!yang)
        yang = ly_ctx_get_module(ctx, "yang", nullptr);
    auto metaValue = [&](const lyd_node *n, const char *name) -> QString {
        if (!yang)
            return {};
        const lyd_meta *m = lyd_find_meta(n->meta, yang, name);
        return m ? QString::fromUtf8(lyd_get_meta_value(m)) : QString();
    };
    std::function<void(const lyd_node *, const QString &)> walk = [&](const lyd_node *first,
                                                                      const QString &inherited) {
        for (const lyd_node *n = first; n && out->size() < kMaxChanges; n = n->next) {
            const QString own = metaValue(n, "operation");
            const QString op = own.isEmpty() ? inherited : own;
            const bool term = n->schema && (n->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST));
            EditChange c;
            c.path = DataTree::path(n);
            if (term)
                c.value = DataTree::value(n);
            bool report = false;
            if (op == QLatin1String("create") && (term || !own.isEmpty())) {
                c.kind = EditChange::Create;
                report = term || !lyd_child(n); // containers: report their leaves instead
            } else if (op == QLatin1String("delete") && !own.isEmpty()) {
                c.kind = EditChange::Delete;
                report = true;
            } else if (op == QLatin1String("replace") && !own.isEmpty()) {
                c.kind = term && !(n->schema->flags & LYS_ORDBY_USER) ? EditChange::Replace
                                                                       : EditChange::Move;
                c.oldValue = metaValue(n, "orig-value");
                report = true;
            }
            if (report)
                out->append(c);
            // deleted subtrees: the top line is enough
            if (!(op == QLatin1String("delete") && !own.isEmpty()))
                walk(lyd_child(n), op);
        }
    };
    walk(diff, QStringLiteral("none"));
}

void validateAndDiff(const ly_ctx *ctx, const lyd_node *base, lyd_node **result,
                     QStringList *errors, QVector<EditChange> *changes)
{
    const int before = int(errors->size());
    LyLogCapture cap(errors);
    if (*result && lyd_validate_all(result, ctx, LYD_VALIDATE_NO_STATE | LYD_VALIDATE_PRESENT, nullptr)
                       != LY_SUCCESS
        && errors->size() == before)
        errors->append(QStringLiteral("the resulting configuration is not valid"));
    lyd_node *diff = nullptr;
    if (lyd_diff_siblings(base, *result, 0, &diff) == LY_SUCCESS && diff)
        collectChanges(ctx, diff, changes);
    lyd_free_all(diff);
}

} // namespace ys::lydutil
