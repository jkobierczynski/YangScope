// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GnmiPath.h"

#include "core/NodeInfo.h"

#include <libyang/libyang.h>

namespace ys::gnmi {

Path Path::joined(const Path &suffix) const
{
    Path out = *this;
    if (out.origin.isEmpty())
        out.origin = suffix.origin;
    if (out.target.isEmpty())
        out.target = suffix.target;
    out.elems += suffix.elems;
    return out;
}

namespace {

QString escapeName(const QString &s)
{
    QString out;
    for (QChar c : s) {
        if (c == QLatin1Char('/') || c == QLatin1Char('[') || c == QLatin1Char(']')
            || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

QString escapeValue(const QString &s)
{
    QString out;
    for (QChar c : s) {
        if (c == QLatin1Char(']') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

} // namespace

bool parsePath(const QString &text, Path *out, QString *error)
{
    *out = Path();
    QString s = text.trimmed();
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    // origin: "origin:/..." where origin contains no '/'
    const qsizetype slash = s.indexOf(QLatin1Char('/'));
    const qsizetype colon = s.indexOf(QLatin1Char(':'));
    if (colon > 0 && (slash < 0 || colon < slash) && colon + 1 < s.size()
        && s.at(colon + 1) == QLatin1Char('/')) {
        out->origin = s.left(colon);
        s = s.mid(colon + 1);
    }
    if (s.isEmpty() || s == QLatin1String("/"))
        return true;

    qsizetype i = 0;
    const qsizetype n = s.size();
    if (s.at(0) == QLatin1Char('/'))
        ++i;
    while (i < n) {
        PathElem e;
        // name, up to '/' or '[' (unescaped)
        while (i < n && s.at(i) != QLatin1Char('/') && s.at(i) != QLatin1Char('[')) {
            if (s.at(i) == QLatin1Char('\\') && i + 1 < n)
                ++i;
            else if (s.at(i) == QLatin1Char(']'))
                return fail(QStringLiteral("unexpected ']' at position %1").arg(i + 1));
            e.name += s.at(i++);
        }
        if (e.name.isEmpty())
            return fail(QStringLiteral("empty path element at position %1").arg(i + 1));
        // keys
        while (i < n && s.at(i) == QLatin1Char('[')) {
            ++i;
            QString key, value;
            while (i < n && s.at(i) != QLatin1Char('=')) {
                if (s.at(i) == QLatin1Char(']'))
                    return fail(QStringLiteral("key without '=' in element \"%1\"").arg(e.name));
                key += s.at(i++);
            }
            if (i >= n)
                return fail(QStringLiteral("unterminated key in element \"%1\"").arg(e.name));
            ++i; // '='
            bool closed = false;
            while (i < n) {
                if (s.at(i) == QLatin1Char('\\') && i + 1 < n) {
                    value += s.at(i + 1);
                    i += 2;
                    continue;
                }
                if (s.at(i) == QLatin1Char(']')) {
                    closed = true;
                    ++i;
                    break;
                }
                value += s.at(i++);
            }
            if (!closed)
                return fail(QStringLiteral("missing ']' after key \"%1\"").arg(key));
            key = key.trimmed();
            if (key.isEmpty())
                return fail(QStringLiteral("empty key name in element \"%1\"").arg(e.name));
            e.keys.append({key, value});
        }
        out->elems.append(e);
        if (i < n) {
            if (s.at(i) != QLatin1Char('/'))
                return fail(QStringLiteral("expected '/' at position %1").arg(i + 1));
            ++i;
            if (i >= n) // trailing slash
                break;
        }
    }
    return true;
}

QString pathToString(const Path &p)
{
    QString s;
    if (!p.origin.isEmpty())
        s += p.origin + QLatin1Char(':');
    if (p.elems.isEmpty())
        return s + QLatin1Char('/');
    for (const PathElem &e : p.elems) {
        s += QLatin1Char('/') + escapeName(e.name);
        for (const auto &k : e.keys)
            s += QLatin1Char('[') + k.first + QLatin1Char('=') + escapeValue(k.second) + QLatin1Char(']');
    }
    return s;
}

Path pathForSchemaNode(const lysc_node *node, bool withModule)
{
    Path p;
    QVector<const lysc_node *> chain;
    for (const lysc_node *n = node; n; n = n->parent)
        if (!(n->nodetype & (LYS_CHOICE | LYS_CASE | LYS_INPUT | LYS_OUTPUT)))
            chain.prepend(n);
    const lysc_node *prev = nullptr;
    for (const lysc_node *n : chain) {
        PathElem e;
        e.name = QString::fromUtf8(n->name);
        if (withModule && (!prev || prev->module != n->module))
            e.name = QString::fromUtf8(n->module->name) + QLatin1Char(':') + e.name;
        if (n->nodetype == LYS_LIST)
            for (const QString &k : listKeys(n))
                e.keys.append({k, QStringLiteral("*")});
        p.elems.append(e);
        prev = n;
    }
    return p;
}


} // namespace ys::gnmi
