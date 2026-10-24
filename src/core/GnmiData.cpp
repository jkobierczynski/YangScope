// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GnmiData.h"

#include "core/DataTree.h"
#include "core/LydUtil.h"
#include "core/NodeInfo.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include <cstdlib>
#include <functional>

namespace ys::gnmi {

using namespace ys::lydutil;

QString encodingName(Encoding e)
{
    switch (e) {
    case Encoding::JSON: return QStringLiteral("JSON");
    case Encoding::Bytes: return QStringLiteral("BYTES");
    case Encoding::Proto: return QStringLiteral("PROTO");
    case Encoding::Ascii: return QStringLiteral("ASCII");
    case Encoding::JsonIetf: return QStringLiteral("JSON_IETF");
    }
    return QStringLiteral("encoding %1").arg(int(e));
}

QString Value::kindName() const
{
    switch (kind) {
    case None: return QStringLiteral("none");
    case String: return QStringLiteral("string");
    case Int: return QStringLiteral("int");
    case Uint: return QStringLiteral("uint");
    case Bool: return QStringLiteral("bool");
    case Bytes: return QStringLiteral("bytes");
    case Double: return QStringLiteral("double");
    case Decimal: return QStringLiteral("decimal");
    case LeafList: return QStringLiteral("leaf-list");
    case Json: return QStringLiteral("json");
    case JsonIetf: return QStringLiteral("json_ietf");
    case Ascii: return QStringLiteral("ascii");
    case Proto: return QStringLiteral("proto");
    case Any: return QStringLiteral("any");
    }
    return {};
}

namespace {

/// "module:name" -> {module, name}
QPair<QString, QString> splitName(const QString &s)
{
    const qsizetype c = s.indexOf(QLatin1Char(':'));
    if (c <= 0)
        return {QString(), s};
    return {s.left(c), s.mid(c + 1)};
}

/// Data children (through choice/case) of @p parent, or top-level data nodes
/// of all implemented modules when @p parent is null.
QVector<const lysc_node *> dataChildren(const ly_ctx *ctx, const lysc_node *parent)
{
    QVector<const lysc_node *> out;
    std::function<void(const lysc_node *)> scan = [&](const lysc_node *first) {
        for (const lysc_node *c = first; c; c = c->next) {
            if (c->nodetype & (LYS_CHOICE | LYS_CASE))
                scan(lysc_node_child(c));
            else if (!(c->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF | LYS_INPUT | LYS_OUTPUT)))
                out << c;
        }
    };
    if (parent) {
        scan(lysc_node_child(parent));
    } else {
        uint32_t idx = 0;
        while (const lys_module *m = ly_ctx_get_module_iter(ctx, &idx))
            if (m->implemented && m->compiled)
                scan(m->compiled->data);
    }
    return out;
}

QString quoted(const QString &v)
{
    if (!v.contains(QLatin1Char('\'')))
        return QLatin1Char('\'') + v + QLatin1Char('\'');
    return QLatin1Char('"') + v + QLatin1Char('"');
}

QString qualified(const lysc_node *n, const lysc_node *parent)
{
    const QString name = QString::fromUtf8(n->name);
    if (!parent || parent->module != n->module)
        return QString::fromUtf8(n->module->name) + QLatin1Char(':') + name;
    return name;
}

bool isWildcard(const QString &v)
{
    return v == QLatin1String("*") || v == QLatin1String("...");
}

/// Parse any JSON text, including bare scalars.
bool parseJsonValue(const QByteArray &text, QJsonValue *out)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson("[" + text + "]", &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1)
        return false;
    *out = doc.array().at(0);
    return true;
}

QByteArray jsonText(const QJsonValue &v, bool indented = false)
{
    if (v.isObject())
        return QJsonDocument(v.toObject()).toJson(indented ? QJsonDocument::Indented : QJsonDocument::Compact).trimmed();
    if (v.isArray())
        return QJsonDocument(v.toArray()).toJson(indented ? QJsonDocument::Indented : QJsonDocument::Compact).trimmed();
    QByteArray a = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return a.mid(1, a.size() - 2);
}

LY_DATA_TYPE leafBaseType(const lysc_node *n)
{
    if (n->nodetype == LYS_LEAF)
        return reinterpret_cast<const lysc_node_leaf *>(n)->type->basetype;
    if (n->nodetype == LYS_LEAFLIST)
        return reinterpret_cast<const lysc_node_leaflist *>(n)->type->basetype;
    return LY_TYPE_UNKNOWN;
}

/// RFC 7951 encoding of one scalar for a leaf of the given type.
QJsonValue typedScalar(LY_DATA_TYPE t, const QJsonValue &v)
{
    const QString text = v.isString() ? v.toString()
                       : v.isBool()   ? (v.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                       : v.isDouble() ? QString::number(v.toDouble(), 'g', 17)
                                      : QString();
    switch (t) {
    case LY_TYPE_INT8: case LY_TYPE_INT16: case LY_TYPE_INT32:
    case LY_TYPE_UINT8: case LY_TYPE_UINT16: case LY_TYPE_UINT32: {
        bool ok = false;
        const double d = text.toDouble(&ok);
        return ok ? QJsonValue(d) : v;
    }
    case LY_TYPE_BOOL:
        if (v.isBool())
            return v;
        if (text == QLatin1String("true") || text == QLatin1String("false"))
            return QJsonValue(text == QLatin1String("true"));
        return v;
    case LY_TYPE_EMPTY:
        return QJsonArray{QJsonValue::Null};
    case LY_TYPE_UNION:
        return v; // let libyang decide
    default:
        if (v.isDouble()) {
            // 64-bit integers and decimal64 are strings in RFC 7951; keep
            // integers integral
            const double d = v.toDouble();
            if (d == double(qint64(d)))
                return QJsonValue(QString::number(qint64(d)));
            return QJsonValue(text);
        }
        if (v.isBool())
            return QJsonValue(text);
        return v;
    }
}

QJsonValue scalarFromValue(const Value &value)
{
    switch (value.kind) {
    case Value::Bool:
        return QJsonValue(value.text == QLatin1String("true"));
    case Value::Int:
    case Value::Uint:
    case Value::Double:
    case Value::Decimal:
        return QJsonValue(value.text); // typed later
    case Value::LeafList: {
        QJsonArray a;
        for (const QString &s : value.list)
            a.append(s);
        return a;
    }
    default:
        return QJsonValue(value.text);
    }
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

Resolved resolve(const ly_ctx *ctx, const Path &path)
{
    Resolved r;
    if (!ctx) {
        r.error = QStringLiteral("no schemas loaded");
        return r;
    }
    const lysc_node *parent = nullptr;
    bool concrete = true;
    QString ly;
    for (int i = 0; i < path.elems.size(); ++i) {
        const PathElem &e = path.elems[i];
        const auto [mod, name] = splitName(e.name);
        if (parent && (parent->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
            r.error = QStringLiteral("\"%1\" is a leaf and has no child \"%2\"")
                          .arg(QString::fromUtf8(parent->name), name);
            return r;
        }
        const lysc_node *match = nullptr;
        const lysc_node *sameModule = nullptr;
        for (const lysc_node *c : dataChildren(ctx, parent)) {
            if (name != QLatin1String(c->name))
                continue;
            if (!mod.isEmpty()) {
                if (mod == QLatin1String(c->module->name)) {
                    match = c;
                    break;
                }
                continue;
            }
            if (!match)
                match = c;
            if (parent && c->module == parent->module && !sameModule)
                sameModule = c;
        }
        if (sameModule)
            match = sameModule;
        if (!match) {
            r.error = parent ? QStringLiteral("no node \"%1\" under %2")
                                   .arg(e.name, pathToString(Path{path.origin, {}, path.elems.mid(0, i)}))
                             : QStringLiteral("no top-level node \"%1\" in the loaded modules").arg(e.name);
            return r;
        }
        r.chain << match;
        ly += QLatin1Char('/') + qualified(match, parent);
        if (match->nodetype == LYS_LIST) {
            const QStringList keys = listKeys(match);
            for (const auto &k : e.keys)
                if (!keys.contains(k.first)) {
                    r.error = QStringLiteral("\"%1\" is not a key of list \"%2\" (keys: %3)")
                                  .arg(k.first, name, keys.join(QStringLiteral(", ")));
                    return r;
                }
            for (const QString &k : keys) {
                QString v;
                bool found = false;
                for (const auto &kv : e.keys)
                    if (kv.first == k) {
                        v = kv.second;
                        found = true;
                    }
                if (!found || isWildcard(v)) {
                    concrete = false;
                    continue;
                }
                ly += QStringLiteral("[%1=%2]").arg(k, quoted(v));
            }
        } else if (!e.keys.isEmpty()) {
            r.error = QStringLiteral("\"%1\" is not a list and takes no keys").arg(name);
            return r;
        }
        parent = match;
    }
    if (concrete)
        r.lyPath = ly;
    return r;
}

bool wrapJson(const ly_ctx *ctx, const Path &path, const Value &value, QByteArray *doc, QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
    QJsonValue v;
    if (value.isJson()) {
        if (!parseJsonValue(value.json, &v))
            return fail(QStringLiteral("value is not valid JSON"));
    } else {
        v = scalarFromValue(value);
    }
    if (path.isRoot()) {
        if (!v.isObject())
            return fail(QStringLiteral("a value for the root path must be a JSON object"));
        *doc = jsonText(v);
        return true;
    }
    const Resolved r = resolve(ctx, path);
    if (!r.ok())
        return fail(r.error);

    const int last = int(r.chain.size()) - 1;
    const lysc_node *node = r.chain[last];
    // Some servers wrap the content in its own name; unwrap it.
    if (v.isObject() && !(node->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
        const QJsonObject o = v.toObject();
        if (o.size() == 1 && splitName(o.begin().key()).second == QLatin1String(node->name)
            && o.begin().value().isObject() == (node->nodetype != LYS_LIST || !path.elems[last].keys.isEmpty()))
            v = o.begin().value();
        else if (o.size() == 1 && splitName(o.begin().key()).second == QLatin1String(node->name)
                 && node->nodetype == LYS_LIST && o.begin().value().isArray())
            v = o.begin().value();
    }

    // the node itself
    if (node->nodetype == LYS_LEAF) {
        v = typedScalar(leafBaseType(node), v.isArray() && v.toArray().size() == 1 ? v.toArray().at(0) : v);
    } else if (node->nodetype == LYS_LEAFLIST) {
        QJsonArray in = v.isArray() ? v.toArray() : QJsonArray{v};
        QJsonArray out;
        for (const QJsonValue &x : in)
            out.append(typedScalar(leafBaseType(node), x));
        v = out;
    }

    auto keysFor = [&](int i) {
        QVector<QPair<QString, QString>> ks;
        for (const auto &kv : path.elems[i].keys)
            if (!isWildcard(kv.second))
                ks << kv;
        return ks;
    };
    auto withKeys = [&](QJsonObject o, int i) {
        const lysc_node *list = r.chain[i];
        for (const auto &kv : keysFor(i)) {
            if (o.contains(kv.first))
                continue;
            // type the key by its leaf
            const lysc_node *keyLeaf = nullptr;
            for (const lysc_node *c = lysc_node_child(list); c; c = c->next)
                if (kv.first == QLatin1String(c->name))
                    keyLeaf = c;
            o.insert(kv.first, keyLeaf ? typedScalar(leafBaseType(keyLeaf), QJsonValue(kv.second))
                                       : QJsonValue(kv.second));
        }
        return o;
    };

    if (node->nodetype == LYS_LIST) {
        if (v.isObject()) {
            v = QJsonArray{withKeys(v.toObject(), last)};
        } else if (!v.isArray()) {
            return fail(QStringLiteral("a list entry needs a JSON object"));
        }
    } else if (node->nodetype == LYS_CONTAINER && !v.isObject()) {
        return fail(QStringLiteral("a container needs a JSON object"));
    }

    // wrap in the ancestors
    for (int i = last; i >= 0; --i) {
        const lysc_node *n = r.chain[i];
        const lysc_node *parent = i > 0 ? r.chain[i - 1] : nullptr;
        QJsonObject o;
        o.insert(qualified(n, parent), v);
        if (i == 0) {
            v = o;
            break;
        }
        if (parent->nodetype == LYS_LIST) {
            if (keysFor(i - 1).size() < listKeys(parent).size())
                return fail(QStringLiteral("list \"%1\" needs all its keys in the path")
                                .arg(QString::fromUtf8(parent->name)));
            v = QJsonArray{withKeys(o, i - 1)};
        } else {
            v = o;
        }
    }
    *doc = jsonText(v);
    return true;
}

int notificationsToTree(const ly_ctx *ctx, const QVector<Notification> &notifications,
                        DataTree *tree, QStringList *messages)
{
    int placed = 0;
    for (const Notification &n : notifications) {
        for (const Path &d : n.deletes)
            messages->append(QStringLiteral("deleted: %1").arg(pathToString(d)));
        for (const Update &u : n.updates) {
            QByteArray doc;
            QString err;
            if (!wrapJson(ctx, u.path, u.value, &doc, &err)) {
                messages->append(QStringLiteral("%1: %2").arg(pathToString(u.path), err));
                continue;
            }
            lyd_node *part = nullptr;
            QStringList ly;
            {
                LyLogCapture cap(&ly);
                if (lyd_parse_data_mem(ctx, doc.constData(), LYD_JSON, LYD_PARSE_ONLY | LYD_PARSE_OPAQ, 0,
                                       &part) != LY_SUCCESS) {
                    lyd_free_all(part);
                    part = nullptr;
                }
            }
            if (!part) {
                messages->append(QStringLiteral("%1: %2").arg(pathToString(u.path),
                                                              ly.isEmpty() ? QStringLiteral("could not parse") : ly.first()));
                continue;
            }
            if (tree->mergeTree(part))
                ++placed;
            else
                messages->append(QStringLiteral("%1: could not merge").arg(pathToString(u.path)));
        }
    }
    return placed;
}

Path pathForDataNode(const lyd_node *node, bool withModule)
{
    Path p;
    QVector<const lyd_node *> chain;
    for (const lyd_node *n = node; n; n = lyd_parent(n))
        chain.prepend(n);
    const lysc_node *prev = nullptr;
    for (const lyd_node *n : chain) {
        const lysc_node *s = n->schema;
        PathElem e;
        if (!s) {
            e.name = DataTree::name(n);
            p.elems << e;
            continue;
        }
        e.name = QString::fromUtf8(s->name);
        if (withModule && (!prev || prev->module != s->module))
            e.name = QString::fromUtf8(s->module->name) + QLatin1Char(':') + e.name;
        if (s->nodetype == LYS_LIST)
            for (const lyd_node *c = lyd_child(n); c; c = c->next)
                if (c->schema && (c->schema->flags & LYS_KEY))
                    e.keys.append({QString::fromUtf8(c->schema->name), DataTree::value(c)});
        p.elems << e;
        prev = s;
    }
    return p;
}

QByteArray entryJson(const lyd_node *node)
{
    if (!node || !node->schema)
        return {};
    if (node->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST)) {
        QJsonValue v = typedScalar(leafBaseType(node->schema), QJsonValue(DataTree::value(node)));
        return jsonText(v);
    }
    lyd_node *dup = nullptr;
    if (lyd_dup_single(node, nullptr, 0x01 /*LYD_DUP_RECURSIVE*/, &dup) != LY_SUCCESS)
        return {};
    if (lyd_node *c = lyd_child(dup)) {
        lyd_node *h = c;
        stripState(&h);
    }
    char *s = nullptr;
    lyd_print_mem(&s, dup, LYD_JSON, 0);
    lyd_free_all(dup);
    const QByteArray printed(s ? s : "");
    std::free(s);
    const QJsonObject o = QJsonDocument::fromJson(printed).object();
    if (o.isEmpty())
        return {};
    QJsonValue v = o.begin().value();
    if (v.isArray() && v.toArray().size() == 1)
        v = v.toArray().at(0);
    return jsonText(v, true);
}

// --- Set ---------------------------------------------------------------------------

bool parseSetScript(const QString &text, QVector<SetOperation> *ops, QStringList *errors)
{
    ops->clear();
    struct Stmt { int line; QString kw; QString body; };
    QVector<Stmt> stmts;
    static const QRegularExpression head(QStringLiteral("^\\s*(update|replace|delete)\\b\\s*(.*)$"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines[i];
        if (l.trimmed().isEmpty() || l.trimmed().startsWith(QLatin1Char('#')))
            continue;
        const auto m = head.match(l);
        if (m.hasMatch()) {
            stmts.append({i + 1, m.captured(1).toLower(), m.captured(2)});
        } else if (!stmts.isEmpty()) {
            stmts.last().body += QLatin1Char('\n') + l;
        } else {
            errors->append(QStringLiteral("line %1: expected update, replace or delete").arg(i + 1));
        }
    }
    for (const Stmt &st : std::as_const(stmts)) {
        SetOperation op;
        op.line = st.line;
        op.kind = st.kw == QLatin1String("delete") ? SetOperation::Delete
                : st.kw == QLatin1String("replace") ? SetOperation::Replace
                                                     : SetOperation::Update;
        QString pathText = st.body.trimmed();
        QString valueText;
        if (op.kind != SetOperation::Delete) {
            // '=' outside [...] separates path and value
            int depth = 0;
            qsizetype eq = -1;
            for (qsizetype i = 0; i < pathText.size(); ++i) {
                const QChar c = pathText.at(i);
                if (c == QLatin1Char('\\')) { ++i; continue; }
                if (c == QLatin1Char('[')) ++depth;
                else if (c == QLatin1Char(']')) depth = qMax(0, depth - 1);
                else if (c == QLatin1Char('=') && depth == 0) { eq = i; break; }
            }
            if (eq < 0) {
                errors->append(QStringLiteral("line %1: %2 needs \"<path> = <value>\"").arg(st.line).arg(st.kw));
                continue;
            }
            valueText = pathText.mid(eq + 1).trimmed();
            pathText = pathText.left(eq).trimmed();
            if (valueText.isEmpty()) {
                errors->append(QStringLiteral("line %1: missing value after '='").arg(st.line));
                continue;
            }
            QJsonValue v;
            if (parseJsonValue(valueText.toUtf8(), &v)) {
                op.json = jsonText(v);
            } else if (!valueText.startsWith(QLatin1Char('{')) && !valueText.startsWith(QLatin1Char('['))
                       && !valueText.startsWith(QLatin1Char('"')) && !valueText.contains(QLatin1Char('\n'))) {
                op.json = jsonText(QJsonValue(valueText)); // bare scalar
            } else {
                errors->append(QStringLiteral("line %1: the value is not valid JSON").arg(st.line));
                continue;
            }
        }
        QString perr;
        if (!parsePath(pathText, &op.path, &perr)) {
            errors->append(QStringLiteral("line %1: %2").arg(st.line).arg(perr));
            continue;
        }
        ops->append(op);
    }
    if (ops->isEmpty() && errors->isEmpty())
        errors->append(QStringLiteral("no operations; write e.g. update /path = value"));
    return errors->isEmpty();
}

QString formatSetOperation(const SetOperation &op)
{
    const char *kw = op.kind == SetOperation::Delete ? "delete"
                   : op.kind == SetOperation::Replace ? "replace" : "update";
    QString s = QString::fromLatin1(kw) + QLatin1Char(' ') + pathToString(op.path);
    if (op.kind == SetOperation::Delete)
        return s;
    QJsonValue v;
    QByteArray text = op.json;
    if (parseJsonValue(op.json, &v))
        text = jsonText(v, true);
    return s + (text.contains('\n') ? QStringLiteral(" =\n") : QStringLiteral(" = ")) + QString::fromUtf8(text);
}

EditValidation validateSet(const ly_ctx *ctx, const QVector<SetOperation> &ops, const DataTree *current)
{
    EditValidation v;
    v.wellFormed = true;
    v.operations = int(ops.size());
    if (!ctx) {
        v.errors << QStringLiteral("no schemas loaded");
        return v;
    }
    struct Prepared { const SetOperation *op; QString lyPath; lyd_node *tree = nullptr; };
    QVector<Prepared> prepared;
    for (const SetOperation &op : ops) {
        Prepared p{&op, {}, nullptr};
        const QString where = QStringLiteral("line %1: ").arg(op.line);
        if (!op.path.isRoot()) {
            const Resolved r = resolve(ctx, op.path);
            if (!r.ok()) {
                v.errors << where + r.error;
                continue;
            }
            if (r.node()->flags & LYS_CONFIG_R) {
                v.errors << where + QStringLiteral("%1 is state data (config false) and cannot be set")
                                        .arg(pathToString(op.path));
                continue;
            }
            if (r.lyPath.isEmpty()) {
                v.errors << where + QStringLiteral("list keys in %1 must be concrete values, not wildcards")
                                        .arg(pathToString(op.path));
                continue;
            }
            p.lyPath = r.lyPath;
        }
        if (op.kind != SetOperation::Delete) {
            Value val;
            val.kind = Value::JsonIetf;
            val.json = op.json;
            QByteArray doc;
            QString err;
            if (!wrapJson(ctx, op.path, val, &doc, &err)) {
                v.errors << where + err;
                continue;
            }
            QStringList ly;
            {
                LyLogCapture cap(&ly);
                if (lyd_parse_data_mem(ctx, doc.constData(), LYD_JSON,
                                       LYD_PARSE_ONLY | LYD_PARSE_STRICT | LYD_PARSE_NO_STATE, 0, &p.tree)
                    != LY_SUCCESS) {
                    lyd_free_all(p.tree);
                    p.tree = nullptr;
                    for (const QString &l : ly)
                        v.errors << where + l;
                    if (ly.isEmpty())
                        v.errors << where + QStringLiteral("the value does not match the schema");
                    continue;
                }
            }
        }
        prepared << p;
    }
    auto freeAll = [&] {
        for (Prepared &p : prepared)
            lyd_free_all(p.tree);
    };
    if (!v.errors.isEmpty()) {
        freeAll();
        return v;
    }
    v.schemaValid = true;
    if (!current) {
        v.warnings << QStringLiteral("checked against the schemas only; mandatory nodes, must/when "
                                     "and leafref targets need the device's current configuration");
        v.ok = true;
        freeAll();
        return v;
    }

    lyd_node *base = nullptr;
    if (!current->isEmpty())
        lyd_dup_siblings(current->root(), nullptr, 0x01, &base);
    if (const int opaque = stripOpaque(&base))
        v.warnings << QStringLiteral("%1 element(s) of the current configuration are not described "
                                     "by the loaded schemas and were left out of the check").arg(opaque);
    lyd_node *result = nullptr;
    if (base)
        lyd_dup_siblings(base, nullptr, 0x01, &result);

    // gNMI order: all deletes, then replaces, then updates
    for (int phase = 0; phase < 3; ++phase) {
        for (Prepared &p : prepared) {
            const SetOperation::Kind k = p.op->kind;
            const bool mine = (phase == 0 && k == SetOperation::Delete)
                              || (phase == 1 && k == SetOperation::Replace)
                              || (phase == 2 && k == SetOperation::Update);
            if (!mine)
                continue;
            if (k == SetOperation::Delete || k == SetOperation::Replace) {
                if (p.op->path.isRoot()) {
                    lyd_free_all(result);
                    result = nullptr;
                } else if (lyd_node *n = findPath(result, p.lyPath)) {
                    freeNode(&result, n);
                } else if (k == SetOperation::Delete) {
                    v.warnings << QStringLiteral("line %1: nothing to delete at %2")
                                      .arg(p.op->line).arg(pathToString(p.op->path));
                }
            }
            if (p.tree) {
                lyd_merge_siblings(&result, p.tree, 0x01 /*LYD_MERGE_DESTRUCT*/);
                p.tree = nullptr;
            }
        }
    }
    v.fullValidation = true;
    validateAndDiff(ctx, base, &result, &v.errors, &v.changes);
    lyd_free_all(base);
    lyd_free_all(result);
    freeAll();
    if (v.changes.isEmpty() && v.errors.isEmpty())
        v.warnings << QStringLiteral("the Set changes nothing in the current configuration");
    v.ok = v.errors.isEmpty();
    return v;
}

} // namespace ys::gnmi
