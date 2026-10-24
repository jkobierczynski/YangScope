// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/EditConfig.h"

#include "core/DataTree.h"
#include "core/LydUtil.h"
#include "core/NetconfXml.h"
#include "core/NodeInfo.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>
#include <libyang/metadata.h>

#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace ys {

using netconf::kBaseNs;
using namespace ys::lydutil;

namespace {

constexpr uint32_t kPrintSiblings = 0x01; // LYD_PRINT_WITHSIBLINGS / LYD_PRINT_SIBLINGS

QString takeString(char *s)
{
    QString out = s ? QString::fromUtf8(s) : QString();
    std::free(s);
    return out;
}

// --- a small DOM of the edit, so nc:operation can be tracked per element ------

enum class Op { Inherit, Merge, Replace, Create, Delete, Remove, None };

Op parseOp(const QString &s, bool *ok)
{
    *ok = true;
    if (s == QLatin1String("merge")) return Op::Merge;
    if (s == QLatin1String("replace")) return Op::Replace;
    if (s == QLatin1String("create")) return Op::Create;
    if (s == QLatin1String("delete")) return Op::Delete;
    if (s == QLatin1String("remove")) return Op::Remove;
    *ok = false;
    return Op::Inherit;
}

struct XNode {
    QString ns, name, text;
    QXmlStreamNamespaceDeclarations decls;
    QXmlStreamAttributes attrs;   // without nc:operation
    Op op = Op::Inherit;          // explicit operation
    Op effective = Op::Merge;
    qint64 line = 0;
    XNode *parent = nullptr;
    const lysc_node *schema = nullptr;
    QString path;                 // instance path, for lyd_find_path
    std::vector<std::unique_ptr<XNode>> kids;
};

bool parseDom(const QByteArray &content, std::vector<std::unique_ptr<XNode>> *top, QStringList *errors)
{
    // The editor holds the children of <config>; supply the wrapper and the
    // usual "nc" prefix so nc:operation needs no declaration of its own.
    const QByteArray wrapped = QByteArrayLiteral("<config xmlns=\"") + kBaseNs.toUtf8()
                               + QByteArrayLiteral("\" xmlns:nc=\"") + kBaseNs.toUtf8()
                               + QByteArrayLiteral("\">") + content + QByteArrayLiteral("</config>");
    QXmlStreamReader r(wrapped);
    std::vector<XNode *> stack;
    int depth = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            ++depth;
            if (depth == 1)
                continue; // our <config>
            auto n = std::make_unique<XNode>();
            n->ns = r.namespaceUri().toString();
            n->name = r.name().toString();
            n->decls = r.namespaceDeclarations();
            n->line = r.lineNumber();
            for (const QXmlStreamAttribute &a : r.attributes()) {
                if (a.namespaceUri() == kBaseNs && a.name() == QLatin1String("operation")) {
                    bool ok = false;
                    n->op = parseOp(a.value().toString(), &ok);
                    if (!ok)
                        errors->append(QStringLiteral("line %1: invalid operation \"%2\" on <%3> "
                                                      "(use merge, replace, create, delete or remove)")
                                           .arg(n->line).arg(a.value().toString(), n->name));
                } else {
                    n->attrs.append(a);
                }
            }
            XNode *raw = n.get();
            if (stack.empty()) {
                top->push_back(std::move(n));
            } else {
                raw->parent = stack.back();
                stack.back()->kids.push_back(std::move(n));
            }
            stack.push_back(raw);
        } else if (r.isEndElement()) {
            --depth;
            if (!stack.empty() && depth >= 1)
                stack.pop_back();
        } else if (r.isCharacters() && !stack.empty()) {
            stack.back()->text += r.text();
        }
    }
    if (r.hasError()) {
        // line numbers are those of the editor (the wrapper adds no newline)
        errors->append(QStringLiteral("line %1: not well-formed XML: %2")
                           .arg(r.lineNumber()).arg(r.errorString()));
        return false;
    }
    return true;
}

/// Data child of @p parent (or top-level node of the module) with this
/// namespace and name, looking through choice/case.
const lysc_node *findDataChild(const ly_ctx *ctx, const lysc_node *parent, const QString &ns,
                               const QString &name)
{
    std::function<const lysc_node *(const lysc_node *)> scan = [&](const lysc_node *first) -> const lysc_node * {
        for (const lysc_node *c = first; c; c = c->next) {
            if (c->nodetype & (LYS_CHOICE | LYS_CASE)) {
                if (const lysc_node *f = scan(lysc_node_child(c)))
                    return f;
                continue;
            }
            if (name == QLatin1String(c->name) && ns == QLatin1String(c->module->ns))
                return c;
        }
        return nullptr;
    };
    if (parent)
        return scan(lysc_node_child(parent));
    const QByteArray nsb = ns.toUtf8();
    const lys_module *mod = ly_ctx_get_module_implemented_ns(ctx, nsb.constData());
    if (!mod || !mod->compiled)
        return nullptr;
    return scan(mod->compiled->data);
}

QString quoted(const QString &v)
{
    if (!v.contains(QLatin1Char('\'')))
        return QLatin1Char('\'') + v + QLatin1Char('\'');
    return QLatin1Char('"') + v + QLatin1Char('"');
}

/// Resolve schema nodes, effective operations and instance paths.
void resolve(const ly_ctx *ctx, XNode *n, Op parentEffective, QStringList *errors)
{
    const lysc_node *parentSchema = n->parent ? n->parent->schema : nullptr;
    if (n->parent && !parentSchema)
        return; // parent already reported
    n->schema = findDataChild(ctx, parentSchema, n->ns, n->name);
    const QString where = n->parent ? n->parent->path : QString();
    if (!n->schema) {
        errors->append(QStringLiteral("line %1: unknown element <%2> (namespace %3)%4")
                           .arg(n->line).arg(n->name, n->ns.isEmpty() ? QStringLiteral("none") : n->ns,
                                where.isEmpty() ? QString() : QStringLiteral(" under ") + where));
        return;
    }
    if (n->schema->flags & LYS_CONFIG_R) {
        errors->append(QStringLiteral("line %1: <%2> is state data (config false) and cannot be edited")
                           .arg(n->line).arg(n->name));
    }
    n->effective = n->op == Op::Inherit ? parentEffective : n->op;

    QString seg;
    if (!n->parent || n->parent->schema->module != n->schema->module)
        seg = QString::fromUtf8(n->schema->module->name) + QLatin1Char(':');
    seg += n->name;
    if (n->schema->nodetype == LYS_LIST) {
        for (const QString &key : listKeys(n->schema)) {
            const XNode *k = nullptr;
            for (const auto &c : n->kids)
                if (c->name == key && c->ns == n->ns)
                    k = c.get();
            if (!k) {
                errors->append(QStringLiteral("line %1: list entry <%2> has no key <%3>")
                                   .arg(n->line).arg(n->name, key));
                seg += QStringLiteral("[%1='']").arg(key);
            } else {
                seg += QStringLiteral("[%1=%2]").arg(key, quoted(k->text.trimmed()));
            }
        }
    } else if (n->schema->nodetype == LYS_LEAFLIST) {
        seg += QStringLiteral("[.=%1]").arg(quoted(n->text.trimmed()));
    }
    n->path = where + QLatin1Char('/') + seg;

    for (const auto &c : n->kids)
        resolve(ctx, c.get(), n->effective, errors);
}

void writeNode(QXmlStreamWriter &w, const XNode *n)
{
    for (const auto &d : n->decls) {
        if (d.prefix().isEmpty())
            w.writeDefaultNamespace(d.namespaceUri().toString());
        else
            w.writeNamespace(d.namespaceUri().toString(), d.prefix().toString());
    }
    w.writeStartElement(n->ns, n->name);
    for (const QXmlStreamAttribute &a : n->attrs) {
        if (a.namespaceUri() == kBaseNs)
            continue;
        if (a.namespaceUri().isEmpty())
            w.writeAttribute(a.name().toString(), a.value().toString());
        else
            w.writeAttribute(a.namespaceUri().toString(), a.name().toString(), a.value().toString());
    }
    if (n->kids.empty())
        w.writeCharacters(n->text.trimmed());
    for (const auto &c : n->kids) {
        // Deleted/removed nodes are only identified by their path; their
        // content (often empty) is not configuration and must not be parsed.
        if (c->effective == Op::Delete || c->effective == Op::Remove)
            continue;
        writeNode(w, c.get());
    }
    w.writeEndElement();
}

QByteArray strippedXml(const std::vector<std::unique_ptr<XNode>> &top)
{
    QByteArray out;
    QXmlStreamWriter w(&out);
    // The base namespace is the default inside <config>; elements without
    // their own declaration land there and will be reported as unknown.
    for (const auto &n : top)
        if (n->effective != Op::Delete && n->effective != Op::Remove)
            writeNode(w, n.get());
    return out;
}

void forEach(XNode *n, const std::function<void(XNode *)> &f)
{
    f(n);
    for (const auto &c : n->kids)
        forEach(c.get(), f);
}

} // namespace

QString EditChange::toString() const
{
    switch (kind) {
    case Create:
        return value.isEmpty() ? QStringLiteral("+ %1").arg(path)
                               : QStringLiteral("+ %1 = %2").arg(path, value);
    case Delete:
        return QStringLiteral("- %1").arg(path);
    case Replace:
        return QStringLiteral("~ %1: %2 -> %3").arg(path, oldValue, value);
    case Move:
        return QStringLiteral("~ %1 (moved)").arg(path);
    }
    return path;
}

EditValidation validateEdit(const ly_ctx *ctx, const QByteArray &configContent,
                            const DataTree *currentConfig, DefaultOperation defaultOp)
{
    EditValidation v;
    if (!ctx) {
        v.errors << QStringLiteral("no schemas loaded");
        return v;
    }
    std::vector<std::unique_ptr<XNode>> top;
    if (!parseDom(configContent, &top, &v.errors))
        return v;
    v.wellFormed = true;
    if (top.empty()) {
        v.errors << QStringLiteral("the edit is empty");
        return v;
    }

    const Op rootOp = defaultOp == DefaultOperation::Replace ? Op::Replace
                    : defaultOp == DefaultOperation::None    ? Op::None
                                                             : Op::Merge;
    for (const auto &n : top)
        resolve(ctx, n.get(), rootOp, &v.errors);
    for (const auto &n : top)
        forEach(n.get(), [&](XNode *x) { v.operations += x->op != Op::Inherit; });
    if (!v.errors.isEmpty())
        return v;

    // 1. The edit on its own: types, ranges, patterns, unknown nodes, state data.
    LyLogCapture cap(&v.errors);
    lyd_node *edit = nullptr;
    const QByteArray stripped = strippedXml(top);
    if (!stripped.isEmpty()
        && lyd_parse_data_mem(ctx, stripped.constData(), LYD_XML,
                              LYD_PARSE_ONLY | LYD_PARSE_STRICT | LYD_PARSE_NO_STATE, 0, &edit)
               != LY_SUCCESS) {
        if (v.errors.isEmpty())
            v.errors << QStringLiteral("the edit does not match the loaded schemas");
        lyd_free_all(edit);
        return v;
    }
    v.schemaValid = true;

    if (!currentConfig) {
        v.warnings << QStringLiteral("checked against the schemas only; mandatory nodes, must/when "
                                     "and leafref targets need the device's current configuration");
        v.ok = v.errors.isEmpty();
        lyd_free_all(edit);
        return v;
    }

    // 2. Apply the edit to a copy of the current configuration.
    lyd_node *base = nullptr;
    if (!currentConfig->isEmpty())
        lyd_dup_siblings(currentConfig->root(), nullptr, LYD_DUP_RECURSIVE, &base);
    if (const int opaque = stripOpaque(&base))
        v.warnings << QStringLiteral("%1 element(s) of the current configuration are not described "
                                     "by the loaded schemas and were left out of the check").arg(opaque);

    lyd_node *result = nullptr;
    if (defaultOp != DefaultOperation::Replace && base)
        lyd_dup_siblings(base, nullptr, LYD_DUP_RECURSIVE, &result);
    if (defaultOp == DefaultOperation::Replace)
        v.warnings << QStringLiteral("default-operation replace: the edit replaces the whole datastore");

    std::vector<XNode *> all;
    for (const auto &n : top)
        forEach(n.get(), [&](XNode *x) { all.push_back(x); });

    // create / delete preconditions are checked against the original
    for (XNode *x : all) {
        if (x->op == Op::Create && findPath(base, x->path))
            v.errors << QStringLiteral("create: %1 already exists (data-exists)").arg(x->path);
        if (x->op == Op::Delete && !findPath(base, x->path))
            v.errors << QStringLiteral("delete: %1 does not exist (data-missing)").arg(x->path);
    }
    // delete / remove / replace drop what is there
    for (XNode *x : all)
        if (x->op == Op::Delete || x->op == Op::Remove || x->op == Op::Replace)
            freeNode(&result, findPath(result, x->path));

    // merge the outermost subtrees that create or merge content
    auto applies = [](Op op) { return op == Op::Merge || op == Op::Replace || op == Op::Create; };
    for (XNode *x : all) {
        if (!applies(x->effective))
            continue;
        if (x->parent && applies(x->parent->effective))
            continue; // part of an outer subtree
        lyd_node *src = findPath(edit, x->path);
        if (!src)
            continue;
        lyd_node *dup = nullptr;
        if (lyd_dup_single(src, nullptr, LYD_DUP_RECURSIVE | LYD_DUP_WITH_PARENTS, &dup) != LY_SUCCESS)
            continue;
        lyd_node *dupRoot = dup;
        while (lyd_parent(dupRoot))
            dupRoot = lyd_parent(dupRoot);
        // nested delete/remove/none parts must not be re-added
        forEach(x, [&](XNode *d) {
            if (d != x && (d->effective == Op::Delete || d->effective == Op::Remove
                           || d->effective == Op::None)
                && d->parent && d->parent->effective != d->effective)
                freeNode(&dupRoot, findPath(dupRoot, d->path));
        });
        lyd_merge_siblings(&result, dupRoot, LYD_MERGE_DESTRUCT);
    }
    lyd_free_all(edit);

    // 3. Validate the resulting configuration.
    v.fullValidation = true;
    const int before = int(v.errors.size());
    if (result && lyd_validate_all(&result, ctx, LYD_VALIDATE_NO_STATE | LYD_VALIDATE_PRESENT, nullptr)
                      != LY_SUCCESS
        && v.errors.size() == before)
        v.errors << QStringLiteral("the resulting configuration is not valid");

    // 4. What would change.
    lyd_node *diff = nullptr;
    if (lyd_diff_siblings(base, result, 0, &diff) == LY_SUCCESS && diff)
        collectChanges(ctx, diff, &v.changes);
    lyd_free_all(diff);
    lyd_free_all(base);
    lyd_free_all(result);

    if (v.changes.isEmpty() && v.errors.isEmpty())
        v.warnings << QStringLiteral("the edit changes nothing in the current configuration");
    v.ok = v.errors.isEmpty();
    return v;
}

QVector<EditChange> diffConfigs(const ly_ctx *ctx, const DataTree &from, const DataTree &to,
                                QStringList *warnings)
{
    QVector<EditChange> out;
    lyd_node *a = nullptr;
    lyd_node *b = nullptr;
    if (!from.isEmpty())
        lyd_dup_siblings(from.root(), nullptr, LYD_DUP_RECURSIVE, &a);
    if (!to.isEmpty())
        lyd_dup_siblings(to.root(), nullptr, LYD_DUP_RECURSIVE, &b);
    const int opaque = stripOpaque(&a) + stripOpaque(&b);
    if (opaque && warnings)
        warnings->append(QStringLiteral("%1 element(s) without a schema were not compared").arg(opaque));
    lyd_node *diff = nullptr;
    if (lyd_diff_siblings(a, b, 0, &diff) == LY_SUCCESS && diff)
        collectChanges(ctx, diff, &out);
    lyd_free_all(diff);
    lyd_free_all(a);
    lyd_free_all(b);
    return out;
}

QString schemaSkeletonXml(const lysc_node *node)
{
    if (!node)
        return {};
    QVector<const lysc_node *> chain;
    for (const lysc_node *n = node; n; n = n->parent)
        if (!(n->nodetype & (LYS_CHOICE | LYS_CASE)))
            chain.prepend(n);
    if (chain.isEmpty() || (chain.first()->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF)))
        return {};
    QString open, close;
    const lysc_node *prev = nullptr;
    for (int i = 0; i < chain.size(); ++i) {
        const lysc_node *n = chain[i];
        const QString name = QString::fromUtf8(n->name);
        QString tag = QLatin1Char('<') + name;
        if (!prev || prev->module != n->module)
            tag += QStringLiteral(" xmlns=\"%1\"").arg(QString::fromUtf8(n->module->ns).toHtmlEscaped());
        const bool last = i == chain.size() - 1;
        if (last && (n->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
            open += tag + QStringLiteral("></") + name + QLatin1Char('>');
            break;
        }
        open += tag + QLatin1Char('>');
        if (n->nodetype == LYS_LIST)
            for (const QString &k : listKeys(n))
                open += QStringLiteral("<%1></%1>").arg(k);
        close.prepend(QStringLiteral("</") + name + QLatin1Char('>'));
        prev = n;
    }
    return netconf::prettyXml((open + close).toUtf8());
}

// --- XML from instance data ---------------------------------------------------

namespace {

QString printNode(const lyd_node *n)
{
    char *s = nullptr;
    lyd_print_mem(&s, n, LYD_XML, LYD_PRINT_SHRINK);
    return takeString(s);
}

/// libyang declares the namespace on every printed root; drop it where the
/// element sits inside a parent of the same namespace.
QString dropRootXmlns(const QString &xml, const char *ns)
{
    const QString decl = QStringLiteral(" xmlns=\"%1\"").arg(QString::fromUtf8(ns).toHtmlEscaped());
    const qsizetype end = xml.indexOf(QLatin1Char('>'));
    const qsizetype at = xml.indexOf(decl);
    if (at < 0 || end < 0 || at > end)
        return xml;
    return xml.left(at) + xml.mid(at + decl.size());
}

QString openTag(const lyd_node *n, const lyd_node *parent, const QString &extraAttrs = {})
{
    QString t = QLatin1Char('<') + QString::fromUtf8(n->schema->name);
    if (!parent || parent->schema->module != n->schema->module)
        t += QStringLiteral(" xmlns=\"%1\"").arg(QString::fromUtf8(n->schema->module->ns).toHtmlEscaped());
    return t + extraAttrs;
}

QString keyLeaves(const lyd_node *list)
{
    QString out;
    for (const lyd_node *c = lyd_child(list); c; c = c->next)
        if (c->schema && c->schema->nodetype == LYS_LEAF && (c->schema->flags & LYS_KEY))
            out += dropRootXmlns(printNode(c), c->schema->module->ns);
    return out;
}

QVector<const lyd_node *> ancestors(const lyd_node *node)
{
    QVector<const lyd_node *> chain;
    for (const lyd_node *p = lyd_parent(node); p; p = lyd_parent(p))
        chain.prepend(p);
    return chain;
}

/// Ancestors with their keys around @p inner.
QString wrapInAncestors(const lyd_node *node, const QString &inner)
{
    const QVector<const lyd_node *> chain = ancestors(node);
    QString open, close;
    const lyd_node *prev = nullptr;
    for (const lyd_node *a : chain) {
        open += openTag(a, prev) + QLatin1Char('>');
        if (a->schema->nodetype == LYS_LIST)
            open += keyLeaves(a);
        close.prepend(QStringLiteral("</") + QString::fromUtf8(a->schema->name) + QLatin1Char('>'));
        prev = a;
    }
    return open + inner + close;
}

void stripState(lyd_node **first)
{
    std::function<void(lyd_node **, lyd_node *)> walk = [&](lyd_node **head, lyd_node *n) {
        while (n) {
            lyd_node *next = n->next;
            if (!n->schema || (n->schema->flags & LYS_CONFIG_R)) {
                freeNode(head, n);
            } else if (lyd_node *c = lyd_child(n)) {
                lyd_node *h = c;
                walk(&h, c);
            }
            n = next;
        }
    };
    walk(first, *first);
}

} // namespace

QString entryEditXml(const lyd_node *node, EntryXml kind)
{
    if (!node || !node->schema)
        return {};
    const lyd_node *parent = lyd_parent(node);
    QString inner;
    if (kind == EntryXml::Config) {
        lyd_node *dup = nullptr;
        if (lyd_dup_single(node, nullptr, LYD_DUP_RECURSIVE, &dup) != LY_SUCCESS)
            return {};
        lyd_node *head = dup;
        if (dup->schema->flags & LYS_CONFIG_R) {
            lyd_free_all(dup);
            return {};
        }
        if (lyd_node *c = lyd_child(dup)) {
            lyd_node *h = c;
            stripState(&h);
        }
        inner = printNode(head);
        lyd_free_all(head);
        if (parent && parent->schema && parent->schema->module == node->schema->module)
            inner = dropRootXmlns(inner, node->schema->module->ns);
        // printNode declares the namespace itself; if the parent has the same
        // namespace that is redundant but harmless.
    } else {
        const QString op = kind == EntryXml::Delete ? QStringLiteral("delete") : QStringLiteral("remove");
        const QString attr = QStringLiteral(" xmlns:nc=\"%1\" nc:operation=\"%2\"").arg(kBaseNs, op);
        const QString name = QString::fromUtf8(node->schema->name);
        if (node->schema->nodetype == LYS_LIST)
            inner = openTag(node, parent, attr) + QLatin1Char('>') + keyLeaves(node)
                    + QStringLiteral("</") + name + QLatin1Char('>');
        else if (node->schema->nodetype == LYS_LEAFLIST)
            inner = openTag(node, parent, attr) + QLatin1Char('>')
                    + DataTree::value(node).toHtmlEscaped() + QStringLiteral("</") + name + QLatin1Char('>');
        else
            inner = openTag(node, parent, attr) + QStringLiteral("/>");
    }
    return netconf::prettyXml(wrapInAncestors(node, inner).toUtf8());
}

QByteArray entrySubtreeFilter(const lyd_node *node)
{
    if (!node || !node->schema)
        return {};
    const lyd_node *parent = lyd_parent(node);
    QString inner;
    const QString name = QString::fromUtf8(node->schema->name);
    if (node->schema->nodetype == LYS_LIST)
        inner = openTag(node, parent) + QLatin1Char('>') + keyLeaves(node) + QStringLiteral("</")
                + name + QLatin1Char('>');
    else if (node->schema->nodetype == LYS_LEAFLIST)
        inner = openTag(node, parent) + QLatin1Char('>') + DataTree::value(node).toHtmlEscaped()
                + QStringLiteral("</") + name + QLatin1Char('>');
    else
        inner = openTag(node, parent) + QStringLiteral("/>");
    return QByteArrayLiteral("<filter type=\"subtree\">") + wrapInAncestors(node, inner).toUtf8()
           + QByteArrayLiteral("</filter>");
}

QString entryXPath(const lyd_node *node, QVector<QPair<QString, QString>> *prefixes)
{
    if (!node || !node->schema)
        return {};
    if (prefixes) {
        prefixes->clear();
        for (const lyd_node *n = node; n; n = lyd_parent(n)) {
            if (!n->schema)
                continue;
            const QString m = QString::fromUtf8(n->schema->module->name);
            bool seen = false;
            for (const auto &p : *prefixes)
                seen |= p.first == m;
            if (!seen)
                prefixes->prepend({m, QString::fromUtf8(n->schema->module->ns)});
        }
    }
    return DataTree::path(node);
}

} // namespace ys
