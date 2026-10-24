// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NodeInfo.h"
#include "core/LyCompat.h"

#include <libyang/libyang.h>

#include <QFile>
#include <QTextStream>

#include <cstdlib>

namespace ys {

namespace {

QString u8(const char *s) { return s ? QString::fromUtf8(s) : QString(); }

// The ly_data_type2str table is not exported by every libyang build, so keep our own.
QString builtinName(LY_DATA_TYPE t)
{
    switch (t) {
    case LY_TYPE_BINARY:  return QStringLiteral("binary");
    case LY_TYPE_UINT8:   return QStringLiteral("uint8");
    case LY_TYPE_UINT16:  return QStringLiteral("uint16");
    case LY_TYPE_UINT32:  return QStringLiteral("uint32");
    case LY_TYPE_UINT64:  return QStringLiteral("uint64");
    case LY_TYPE_STRING:  return QStringLiteral("string");
    case LY_TYPE_BITS:    return QStringLiteral("bits");
    case LY_TYPE_BOOL:    return QStringLiteral("boolean");
    case LY_TYPE_DEC64:   return QStringLiteral("decimal64");
    case LY_TYPE_EMPTY:   return QStringLiteral("empty");
    case LY_TYPE_ENUM:    return QStringLiteral("enumeration");
    case LY_TYPE_IDENT:   return QStringLiteral("identityref");
    case LY_TYPE_INST:    return QStringLiteral("instance-identifier");
    case LY_TYPE_LEAFREF: return QStringLiteral("leafref");
    case LY_TYPE_UNION:   return QStringLiteral("union");
    case LY_TYPE_INT8:    return QStringLiteral("int8");
    case LY_TYPE_INT16:   return QStringLiteral("int16");
    case LY_TYPE_INT32:   return QStringLiteral("int32");
    case LY_TYPE_INT64:   return QStringLiteral("int64");
    default:              return QStringLiteral("unknown");
    }
}
QString esc(const QString &s) { return s.toHtmlEscaped(); }
QString esc(const char *s) { return u8(s).toHtmlEscaped(); }

bool isSchemaOnly(const lysc_node *n)
{
    return n->nodetype & (LYS_CHOICE | LYS_CASE);
}

bool isInOut(const lysc_node *n)
{
    return n->nodetype & (LYS_INPUT | LYS_OUTPUT);
}

QString takeLyString(char *s)
{
    QString out = u8(s);
    std::free(s);
    return out;
}

/// Ancestors from the top down to (and including) @p node, skipping nodes
/// that never appear in instance data paths.
QVector<const lysc_node *> dataLineage(const lysc_node *node, bool keepInOut = false)
{
    QVector<const lysc_node *> chain;
    for (const lysc_node *n = node; n; n = n->parent) {
        if (isSchemaOnly(n))
            continue;
        if (!keepInOut && isInOut(n))
            continue;
        chain.prepend(n);
    }
    return chain;
}

bool underOperation(const lysc_node *node)
{
    for (const lysc_node *n = node; n; n = n->parent)
        if (n->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF))
            return true;
    return false;
}

const lysp_type *parsedType(const lysc_node *node)
{
    if (!node->priv)
        return nullptr;
    if (node->nodetype == LYS_LEAF)
        return &static_cast<const lysp_node_leaf *>(node->priv)->type;
    if (node->nodetype == LYS_LEAFLIST)
        return &static_cast<const lysp_node_leaflist *>(node->priv)->type;
    return nullptr;
}

const lysc_type *compiledType(const lysc_node *node)
{
    if (node->nodetype == LYS_LEAF)
        return reinterpret_cast<const lysc_node_leaf *>(node)->type;
    if (node->nodetype == LYS_LEAFLIST)
        return reinterpret_cast<const lysc_node_leaflist *>(node)->type;
    return nullptr;
}

bool isUnsigned(LY_DATA_TYPE t)
{
    return t == LY_TYPE_UINT8 || t == LY_TYPE_UINT16 || t == LY_TYPE_UINT32
        || t == LY_TYPE_UINT64 || t == LY_TYPE_STRING || t == LY_TYPE_BINARY;
}

QString decimalString(int64_t v, uint8_t digits)
{
    if (digits == 0)
        return QString::number(v);
    const bool neg = v < 0;
    QString s = QString::number(neg ? -(v + 1) + 1ULL : uint64_t(v)); // avoid overflow on INT64_MIN
    while (s.size() <= digits)
        s.prepend(QLatin1Char('0'));
    s.insert(s.size() - digits, QLatin1Char('.'));
    return neg ? QLatin1Char('-') + s : s;
}

QString rangeString(const lysc_range *r, LY_DATA_TYPE base, uint8_t fractionDigits = 0)
{
    if (!r || !r->parts)
        return {};
    QStringList parts;
    for (uint64_t i = 0; i < lyCount(r->parts); ++i) {
        const auto &p = r->parts[i];
        QString lo, hi;
        if (base == LY_TYPE_DEC64) {
            lo = decimalString(p.min_64, fractionDigits);
            hi = decimalString(p.max_64, fractionDigits);
        } else if (isUnsigned(base)) {
            lo = QString::number(p.min_u64);
            hi = QString::number(p.max_u64);
        } else {
            lo = QString::number(p.min_64);
            hi = QString::number(p.max_64);
        }
        parts << (lo == hi ? lo : lo + QStringLiteral("..") + hi);
    }
    return parts.join(QStringLiteral(" | "));
}

QString identName(const lysc_ident *id)
{
    return u8(id->module->name) + QLatin1Char(':') + u8(id->name);
}

void row(QString &html, const QString &label, const QString &valueHtml)
{
    if (valueHtml.isEmpty())
        return;
    html += QStringLiteral("<tr><th align='left' valign='top' style='padding-right:12px;"
                           "white-space:nowrap'>%1</th><td>%2</td></tr>")
                .arg(esc(label), valueHtml);
}

QString code(const QString &s)
{
    return s.isEmpty() ? QString() : QStringLiteral("<code>%1</code>").arg(esc(s));
}

QString pre(const QString &s)
{
    return QStringLiteral("<pre style='margin:2px 0'>%1</pre>").arg(esc(s));
}

/// Describe the type of a leaf/leaf-list in HTML rows.
void typeRows(QString &html, const lysc_node *node)
{
    const lysc_type *t = compiledType(node);
    if (!t)
        return;
    const lysp_type *pt = parsedType(node);
    QString typeLine = code(typeName(node));
    if (pt && QString(u8(pt->name)) != baseTypeName(node))
        typeLine += QStringLiteral(" &rarr; ") + code(baseTypeName(node));
    row(html, QStringLiteral("Type"), typeLine);

    switch (t->basetype) {
    case LY_TYPE_INT8: case LY_TYPE_INT16: case LY_TYPE_INT32: case LY_TYPE_INT64:
    case LY_TYPE_UINT8: case LY_TYPE_UINT16: case LY_TYPE_UINT32: case LY_TYPE_UINT64: {
        auto *n = reinterpret_cast<const lysc_type_num *>(t);
        row(html, QStringLiteral("Range"), code(rangeString(n->range, t->basetype)));
        break;
    }
    case LY_TYPE_DEC64: {
        auto *d = reinterpret_cast<const lysc_type_dec *>(t);
        row(html, QStringLiteral("Fraction digits"), QString::number(d->fraction_digits));
        row(html, QStringLiteral("Range"),
            code(rangeString(d->range, t->basetype, d->fraction_digits)));
        break;
    }
    case LY_TYPE_STRING: {
        auto *s = reinterpret_cast<const lysc_type_str *>(t);
        row(html, QStringLiteral("Length"), code(rangeString(s->length, t->basetype)));
        QStringList pats;
        for (uint64_t i = 0; i < lyCount(s->patterns); ++i) {
            const lysc_pattern *p = s->patterns[i];
            pats << (p->inverted ? QStringLiteral("NOT ") : QString()) + u8(p->expr);
        }
        if (!pats.isEmpty())
            row(html, QStringLiteral("Pattern"), pre(pats.join(QLatin1Char('\n'))));
        break;
    }
    case LY_TYPE_BINARY: {
        auto *b = reinterpret_cast<const lysc_type_bin *>(t);
        row(html, QStringLiteral("Length"), code(rangeString(b->length, t->basetype)));
        break;
    }
    case LY_TYPE_ENUM: {
        auto *e = reinterpret_cast<const lysc_type_enum *>(t);
        QString list = QStringLiteral("<table cellspacing='0'>");
        for (uint64_t i = 0; i < lyCount(e->enums); ++i) {
            const lysc_type_bitenum_item &it = e->enums[i];
            list += QStringLiteral("<tr><td><code>%1</code></td><td style='padding-left:8px'>%2</td>"
                                   "<td style='padding-left:8px;color:gray'>%3</td></tr>")
                        .arg(esc(it.name)).arg(it.value)
                        .arg(esc(u8(it.dsc).simplified().left(160)));
        }
        list += QStringLiteral("</table>");
        row(html, QStringLiteral("Enum values"), list);
        break;
    }
    case LY_TYPE_BITS: {
        auto *b = reinterpret_cast<const lysc_type_bits *>(t);
        QStringList bits;
        for (uint64_t i = 0; i < lyCount(b->bits); ++i)
            bits << QStringLiteral("%1 (%2)").arg(u8(b->bits[i].name)).arg(b->bits[i].position);
        row(html, QStringLiteral("Bits"), esc(bits.join(QStringLiteral(", "))));
        break;
    }
    case LY_TYPE_IDENT: {
        auto *id = reinterpret_cast<const lysc_type_identityref *>(t);
        QStringList bases;
        for (uint64_t i = 0; i < lyCount(id->bases); ++i)
            bases << identName(id->bases[i]);
        row(html, QStringLiteral("Identity base"), code(bases.join(QStringLiteral(", "))));
        // Show the directly derived identities as the usable values.
        QStringList derived;
        for (uint64_t i = 0; i < lyCount(id->bases); ++i) {
            const lysc_ident *base = id->bases[i];
            for (uint64_t j = 0; j < lyCount(base->derived); ++j)
                derived << identName(base->derived[j]);
        }
        if (!derived.isEmpty())
            row(html, QStringLiteral("Derived identities"),
                esc(derived.join(QStringLiteral(", "))));
        break;
    }
    case LY_TYPE_LEAFREF: {
        auto *lr = reinterpret_cast<const lysc_type_leafref *>(t);
        row(html, QStringLiteral("Leafref path"), code(u8(lyxp_get_expr(lr->path))));
        if (lr->realtype)
            row(html, QStringLiteral("Target type"),
                code(builtinName(lr->realtype->basetype)));
        row(html, QStringLiteral("Require instance"),
            lr->require_instance ? QStringLiteral("true") : QStringLiteral("false"));
        break;
    }
    case LY_TYPE_UNION: {
        auto *u = reinterpret_cast<const lysc_type_union *>(t);
        QStringList members;
        for (uint64_t i = 0; i < lyCount(u->types); ++i)
            members << builtinName(u->types[i]->basetype);
        // Prefer the member type names as written, if available.
        if (pt && pt->types) {
            QStringList written;
            for (uint64_t i = 0; i < lyCount(pt->types); ++i)
                written << u8(pt->types[i].name);
            if (!written.isEmpty())
                members = written;
        }
        row(html, QStringLiteral("Union of"), code(members.join(QStringLiteral(" | "))));
        break;
    }
    default:
        break;
    }
}

QString qualifiedName(const lysc_node *node, const lysc_node *prev)
{
    // RFC 7951 style: prefix with the module name where the namespace changes.
    const bool needPrefix = !prev || prev->module != node->module;
    return needPrefix ? u8(node->module->name) + QLatin1Char(':') + u8(node->name)
                      : u8(node->name);
}

} // namespace

// ---------------------------------------------------------------------------

QVector<const lysc_node *> childrenOf(const lysc_node *node)
{
    QVector<const lysc_node *> out;
    if (!node)
        return out;
    for (const lysc_node *c = lysc_node_child(node); c; c = c->next) {
        // Hide empty input/output like pyang does.
        if (isInOut(c) && !lysc_node_child(c))
            continue;
        out << c;
    }
    if (node->nodetype & (LYS_CONTAINER | LYS_LIST)) {
        for (const lysc_node_action *a = lysc_node_actions(node); a; a = a->next)
            out << &a->node;
        for (const lysc_node_notif *n = lysc_node_notifs(node); n; n = n->next)
            out << &n->node;
    }
    return out;
}

QVector<const lysc_node *> topLevelOf(const lys_module *mod)
{
    QVector<const lysc_node *> out;
    if (!mod || !mod->compiled)
        return out;
    for (const lysc_node *n = mod->compiled->data; n; n = n->next)
        out << n;
    for (const lysc_node_action *a = mod->compiled->rpcs; a; a = a->next)
        out << &a->node;
    for (const lysc_node_notif *n = mod->compiled->notifs; n; n = n->next)
        out << &n->node;
    return out;
}

QString nodeKind(const lysc_node *node)
{
    switch (node->nodetype) {
    case LYS_CONTAINER: return QStringLiteral("container");
    case LYS_CHOICE:    return QStringLiteral("choice");
    case LYS_CASE:      return QStringLiteral("case");
    case LYS_LEAF:      return QStringLiteral("leaf");
    case LYS_LEAFLIST:  return QStringLiteral("leaf-list");
    case LYS_LIST:      return QStringLiteral("list");
    case LYS_ANYXML:    return QStringLiteral("anyxml");
    case LYS_ANYDATA:   return QStringLiteral("anydata");
    case LYS_RPC:       return QStringLiteral("rpc");
    case LYS_ACTION:    return QStringLiteral("action");
    case LYS_NOTIF:     return QStringLiteral("notification");
    case LYS_INPUT:     return QStringLiteral("input");
    case LYS_OUTPUT:    return QStringLiteral("output");
    default:            return u8(lys_nodetype2str(node->nodetype));
    }
}

QString baseTypeName(const lysc_node *node)
{
    const lysc_type *t = compiledType(node);
    return t ? builtinName(t->basetype) : QString();
}

QString typeName(const lysc_node *node)
{
    if (const lysp_type *pt = parsedType(node))
        if (pt->name)
            return u8(pt->name);
    return baseTypeName(node);
}

Access access(const lysc_node *node)
{
    switch (node->nodetype) {
    case LYS_RPC:
    case LYS_ACTION:
        return Access::Exec;
    case LYS_NOTIF:
        return Access::Notif;
    case LYS_INPUT:
    case LYS_OUTPUT:
        return Access::None;
    default:
        break;
    }
    for (const lysc_node *p = node->parent; p; p = p->parent) {
        if (p->nodetype == LYS_INPUT)
            return Access::Input;
        if (p->nodetype & (LYS_OUTPUT | LYS_NOTIF))
            return Access::ReadOnly;
    }
    if (node->flags & LYS_CONFIG_W)
        return Access::ReadWrite;
    if (node->flags & LYS_CONFIG_R)
        return Access::ReadOnly;
    return Access::None;
}

QString accessShort(Access a)
{
    switch (a) {
    case Access::ReadWrite: return QStringLiteral("rw");
    case Access::ReadOnly:  return QStringLiteral("ro");
    case Access::Input:     return QStringLiteral("-w");
    case Access::Exec:      return QStringLiteral("-x");
    case Access::Notif:     return QStringLiteral("-n");
    case Access::None:      break;
    }
    return {};
}

QStringList listKeys(const lysc_node *node)
{
    QStringList keys;
    if (!node || node->nodetype != LYS_LIST)
        return keys;
    for (const lysc_node *c = lysc_node_child(node); c; c = c->next)
        if (c->nodetype == LYS_LEAF && (c->flags & LYS_KEY))
            keys << u8(c->name);
    return keys;
}

bool isMandatory(const lysc_node *node)
{
    if (node->flags & LYS_MAND_TRUE)
        return true;
    if (node->nodetype == LYS_LEAF && (node->flags & LYS_KEY))
        return true;
    if (node->nodetype == LYS_LIST)
        return reinterpret_cast<const lysc_node_list *>(node)->min > 0;
    if (node->nodetype == LYS_LEAFLIST)
        return reinterpret_cast<const lysc_node_leaflist *>(node)->min > 0;
    return false;
}

bool isDeprecated(const lysc_node *node) { return node->flags & LYS_STATUS_DEPRC; }
bool isObsolete(const lysc_node *node) { return node->flags & LYS_STATUS_OBSLT; }

QString flagsShort(const lysc_node *node)
{
    switch (node->nodetype) {
    case LYS_LIST:
    case LYS_LEAFLIST:
        return QStringLiteral("*");
    case LYS_CONTAINER:
        return (node->flags & LYS_PRESENCE) ? QStringLiteral("!") : QString();
    case LYS_LEAF:
    case LYS_CHOICE:
    case LYS_ANYDATA:
    case LYS_ANYXML:
        return isMandatory(node) ? QString() : QStringLiteral("?");
    default:
        return {};
    }
}

// --- paths -----------------------------------------------------------------

QString schemaPath(const lysc_node *node)
{
    return takeLyString(lysc_path(node, LYSC_PATH_LOG, nullptr, 0));
}

QString dataPath(const lysc_node *node)
{
    return takeLyString(lysc_path(node, LYSC_PATH_DATA, nullptr, 0));
}

QString dataPathWithKeys(const lysc_node *node)
{
    QString p = takeLyString(lysc_path(node, LYSC_PATH_DATA_PATTERN, nullptr, 0));
    p.replace(QStringLiteral("%s"), QStringLiteral("*"));
    return p;
}

QString gnmiPath(const lysc_node *node, bool originPrefix)
{
    if (underOperation(node))
        return {};
    QString out;
    const lysc_node *prev = nullptr;
    for (const lysc_node *n : dataLineage(node)) {
        out += QLatin1Char('/');
        out += originPrefix ? qualifiedName(n, prev) : u8(n->name);
        if (n->nodetype == LYS_LIST)
            for (const QString &k : listKeys(n))
                out += QStringLiteral("[%1=*]").arg(k);
        prev = n;
    }
    return out;
}

QString restconfPath(const lysc_node *node)
{
    const QVector<const lysc_node *> chain = dataLineage(node);
    if (chain.isEmpty())
        return {};
    const bool isOp = chain.first()->nodetype == LYS_RPC;
    QString out = isOp ? QStringLiteral("/restconf/operations") : QStringLiteral("/restconf/data");
    const lysc_node *prev = nullptr;
    for (const lysc_node *n : chain) {
        out += QLatin1Char('/') + qualifiedName(n, prev);
        if (n->nodetype == LYS_LIST) {
            QStringList ks;
            for (const QString &k : listKeys(n))
                ks << QLatin1Char('{') + k + QLatin1Char('}');
            if (!ks.isEmpty())
                out += QLatin1Char('=') + ks.join(QLatin1Char(','));
        }
        prev = n;
    }
    return out;
}

QString subtreeFilter(const lysc_node *node)
{
    if (underOperation(node))
        return {};
    const QVector<const lysc_node *> chain = dataLineage(node);
    QString out = QStringLiteral("<filter type=\"subtree\">\n");
    QString indent = QStringLiteral("  ");
    const lysc_node *prev = nullptr;
    for (int i = 0; i < chain.size(); ++i) {
        const lysc_node *n = chain[i];
        QString open = QStringLiteral("<") + u8(n->name);
        if (!prev || prev->module != n->module)
            open += QStringLiteral(" xmlns=\"%1\"").arg(u8(n->module->ns));
        const bool last = i == chain.size() - 1;
        out += indent + open + (last ? QStringLiteral("/>\n") : QStringLiteral(">\n"));
        if (!last)
            indent += QStringLiteral("  ");
        prev = n;
    }
    for (int i = chain.size() - 2; i >= 0; --i) {
        indent.chop(2);
        out += indent + QStringLiteral("</") + u8(chain[i]->name) + QStringLiteral(">\n");
    }
    out += QStringLiteral("</filter>");
    return out;
}

// --- rendering --------------------------------------------------------------

QString nodeDetailsHtml(const lysc_node *node)
{
    if (!node)
        return {};
    QString h;
    h += QStringLiteral("<h2 style='margin-bottom:2px'>%1 <span style='font-weight:normal;"
                        "color:gray'>%2</span></h2>")
             .arg(esc(node->name), esc(nodeKind(node)));
    if (isObsolete(node))
        h += QStringLiteral("<p style='color:#b00'><b>status obsolete</b></p>");
    else if (isDeprecated(node))
        h += QStringLiteral("<p style='color:#b60'><b>status deprecated</b></p>");

    h += QStringLiteral("<table cellspacing='2'>");
    const lys_module *m = node->module;
    row(h, QStringLiteral("Module"),
        esc(QStringLiteral("%1%2  (prefix %3)")
                .arg(u8(m->name),
                     m->revision ? QLatin1Char('@') + u8(m->revision) : QString(),
                     u8(m->prefix))));
    row(h, QStringLiteral("Namespace"), code(u8(m->ns)));

    const Access a = access(node);
    static const char *accessNames[] = {"read-write (config true)", "read-only (state)",
                                        "input parameter", "executable", "notification", ""};
    row(h, QStringLiteral("Access"), esc(accessNames[int(a)]));

    typeRows(h, node);

    if (node->nodetype == LYS_LEAF) {
        auto *pl = static_cast<const lysp_node_leaf *>(node->priv);
        auto *cl = reinterpret_cast<const lysc_node_leaf *>(node);
        row(h, QStringLiteral("Units"), esc(cl->units));
        if (pl && pl->dflt.str)
            row(h, QStringLiteral("Default"), code(u8(pl->dflt.str)));
        if (node->flags & LYS_KEY)
            row(h, QStringLiteral("Key"), QStringLiteral("yes"));
    } else if (node->nodetype == LYS_LEAFLIST) {
        auto *pl = static_cast<const lysp_node_leaflist *>(node->priv);
        auto *cl = reinterpret_cast<const lysc_node_leaflist *>(node);
        row(h, QStringLiteral("Units"), esc(cl->units));
        if (pl && pl->dflts) {
            QStringList d;
            for (uint64_t i = 0; i < lyCount(pl->dflts); ++i)
                d << u8(pl->dflts[i].str);
            row(h, QStringLiteral("Defaults"), code(d.join(QStringLiteral(", "))));
        }
        row(h, QStringLiteral("Elements"),
            esc(QStringLiteral("min %1, max %2").arg(cl->min)
                    .arg(cl->max == 0xFFFFFFFF || cl->max == 0 ? QStringLiteral("unbounded")
                                                               : QString::number(cl->max))));
        if (node->flags & LYS_ORDBY_USER)
            row(h, QStringLiteral("Ordered by"), QStringLiteral("user"));
    } else if (node->nodetype == LYS_LIST) {
        auto *cl = reinterpret_cast<const lysc_node_list *>(node);
        const QStringList keys = listKeys(node);
        row(h, QStringLiteral("Keys"), keys.isEmpty() ? QStringLiteral("<i>none (keyless list)</i>")
                                                      : code(keys.join(QLatin1Char(' '))));
        row(h, QStringLiteral("Elements"),
            esc(QStringLiteral("min %1, max %2").arg(cl->min)
                    .arg(cl->max == 0xFFFFFFFF || cl->max == 0 ? QStringLiteral("unbounded")
                                                               : QString::number(cl->max))));
        if (node->flags & LYS_ORDBY_USER)
            row(h, QStringLiteral("Ordered by"), QStringLiteral("user"));
    } else if (node->nodetype == LYS_CONTAINER && (node->flags & LYS_PRESENCE)) {
        auto *pc = static_cast<const lysp_node_container *>(node->priv);
        row(h, QStringLiteral("Presence"),
            pc && pc->presence ? esc(u8(pc->presence).simplified()) : QStringLiteral("yes"));
    }

    if (node->nodetype & (LYS_LEAF | LYS_CHOICE | LYS_ANYDATA | LYS_ANYXML))
        row(h, QStringLiteral("Mandatory"), isMandatory(node) ? QStringLiteral("yes")
                                                              : QStringLiteral("no"));

    // when / must
    if (lysc_when **whens = lysc_node_when(node)) {
        QStringList w;
        for (uint64_t i = 0; i < lyCount(whens); ++i)
            w << u8(lyxp_get_expr(whens[i]->cond));
        if (!w.isEmpty())
            row(h, QStringLiteral("When"), pre(w.join(QLatin1Char('\n'))));
    }
    if (lysc_must *musts = lysc_node_musts(node)) {
        QStringList ms;
        for (uint64_t i = 0; i < lyCount(musts); ++i) {
            QString s = u8(lyxp_get_expr(musts[i].cond));
            if (musts[i].emsg)
                s += QStringLiteral("\n    error-message: ") + u8(musts[i].emsg);
            ms << s;
        }
        if (!ms.isEmpty())
            row(h, QStringLiteral("Must"), pre(ms.join(QLatin1Char('\n'))));
    }
    // if-feature as written
    if (node->priv && !(node->nodetype & (LYS_INPUT | LYS_OUTPUT))) {
        auto *pn = static_cast<const lysp_node *>(node->priv);
        QStringList feats;
        for (uint64_t i = 0; i < lyCount(pn->iffeatures); ++i)
            feats << u8(pn->iffeatures[i].str);
        if (!feats.isEmpty())
            row(h, QStringLiteral("If-feature"), code(feats.join(QStringLiteral(", "))));
    }
    h += QStringLiteral("</table>");

    if (node->dsc)
        h += QStringLiteral("<h3>Description</h3><p style='white-space:pre-wrap'>%1</p>")
                 .arg(esc(node->dsc));
    if (node->ref)
        h += QStringLiteral("<h3>Reference</h3><p style='white-space:pre-wrap'>%1</p>")
                 .arg(esc(node->ref));

    h += QStringLiteral("<h3>Paths</h3><table cellspacing='2'>");
    row(h, QStringLiteral("Schema node id"), code(schemaPath(node)));
    row(h, QStringLiteral("XPath"), code(dataPath(node)));
    const QString withKeys = dataPathWithKeys(node);
    if (withKeys != dataPath(node))
        row(h, QStringLiteral("XPath (keys)"), code(withKeys));
    row(h, QStringLiteral("gNMI path"), code(gnmiPath(node, false)));
    row(h, QStringLiteral("gNMI (rfc7951)"), code(gnmiPath(node, true)));
    row(h, QStringLiteral("RESTCONF"), code(restconfPath(node)));
    h += QStringLiteral("</table>");

    const QString filter = subtreeFilter(node);
    if (!filter.isEmpty())
        h += QStringLiteral("<h3>NETCONF subtree filter</h3>") + pre(filter);
    return h;
}

QString moduleDetailsHtml(const lys_module *mod)
{
    if (!mod)
        return {};
    QString h;
    h += QStringLiteral("<h2 style='margin-bottom:2px'>%1 <span style='font-weight:normal;"
                        "color:gray'>module</span></h2><table cellspacing='2'>")
             .arg(esc(mod->name));
    row(h, QStringLiteral("Revision"), esc(mod->revision));
    row(h, QStringLiteral("Prefix"), code(u8(mod->prefix)));
    row(h, QStringLiteral("Namespace"), code(u8(mod->ns)));
    row(h, QStringLiteral("Organization"), esc(u8(mod->org).simplified()));
    row(h, QStringLiteral("Contact"), QStringLiteral("<span style='white-space:pre-wrap'>%1</span>")
                                          .arg(esc(u8(mod->contact).trimmed())));
    row(h, QStringLiteral("File"), code(u8(mod->filepath)));
    row(h, QStringLiteral("Implemented"), mod->implemented ? QStringLiteral("yes")
                                                           : QStringLiteral("no (import only)"));

    if (mod->parsed) {
        QStringList imports, includes, feats;
        for (uint64_t i = 0; i < lyCount(mod->parsed->imports); ++i) {
            const lysp_import &imp = mod->parsed->imports[i];
            imports << u8(imp.name) + (imp.rev[0] ? QLatin1Char('@') + u8(imp.rev) : QString());
        }
        for (uint64_t i = 0; i < lyCount(mod->parsed->includes); ++i)
            includes << u8(mod->parsed->includes[i].name);
        for (uint64_t i = 0; i < lyCount(mod->parsed->features); ++i) {
            const lysp_feature &f = mod->parsed->features[i];
            feats << esc(f.name) + ((f.flags & LYS_FENABLED)
                                        ? QStringLiteral(" <span style='color:green'>&#10003;</span>")
                                        : QStringLiteral(" <span style='color:gray'>(off)</span>"));
        }
        row(h, QStringLiteral("Imports"), esc(imports.join(QStringLiteral(", "))));
        row(h, QStringLiteral("Includes"), esc(includes.join(QStringLiteral(", "))));
        row(h, QStringLiteral("Features"), feats.join(QStringLiteral(", ")));
    }
    h += QStringLiteral("</table>");
    if (mod->dsc)
        h += QStringLiteral("<h3>Description</h3><p style='white-space:pre-wrap'>%1</p>")
                 .arg(esc(mod->dsc));
    return h;
}

QString moduleTreeText(const lys_module *mod, int lineLength)
{
    if (!mod)
        return {};
    char *buf = nullptr;
    ly_out *out = nullptr;
    if (ly_out_new_memory(&buf, 0, &out) != LY_SUCCESS)
        return {};
    lys_print_module(out, mod, LYS_OUT_TREE, size_t(lineLength), 0);
    ly_out_free(out, nullptr, 0);
    return takeLyString(buf);
}

QString moduleSource(const lys_module *mod)
{
    if (!mod)
        return {};
    if (mod->filepath) {
        QFile f(u8(mod->filepath));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString::fromUtf8(f.readAll());
    }
    char *buf = nullptr;
    if (lys_print_mem(&buf, mod, LYS_OUT_YANG, 0) != LY_SUCCESS)
        return {};
    return takeLyString(buf);
}

} // namespace ys
