// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/YangHighlighter.h"

#include <iterator>

namespace ys {

namespace {

const char *const kStructural[] = {
    "module", "submodule", "container", "list", "leaf", "leaf-list", "choice", "case",
    "grouping", "uses", "augment", "rpc", "action", "notification", "input", "output",
    "anydata", "anyxml", "typedef", "identity", "feature", "extension", "deviation",
    "deviate", "refine",
};

const char *const kStatements[] = {
    "yang-version", "namespace", "prefix", "import", "include", "belongs-to",
    "organization", "contact", "description", "reference", "revision", "revision-date",
    "type", "units", "default", "config", "mandatory", "presence", "key", "unique",
    "min-elements", "max-elements", "ordered-by", "must", "when", "if-feature", "status",
    "base", "range", "length", "pattern", "enum", "bit", "value", "position", "path",
    "require-instance", "fraction-digits", "error-message", "error-app-tag", "argument",
    "yin-element", "modifier",
};

const char *const kBuiltinTypes[] = {
    "binary", "bits", "boolean", "decimal64", "empty", "enumeration", "identityref",
    "instance-identifier", "int8", "int16", "int32", "int64", "leafref", "string",
    "uint8", "uint16", "uint32", "uint64", "union", "true", "false",
};

QString wordAlternation(const char *const *words, size_t n)
{
    QStringList parts;
    for (size_t i = 0; i < n; ++i)
        parts << QRegularExpression::escape(QString::fromLatin1(words[i]));
    // keywords may contain '-', so use explicit boundaries
    return QStringLiteral("(?<![\\w\\-:])(") + parts.join(QLatin1Char('|'))
           + QStringLiteral(")(?![\\w\\-:])");
}

} // namespace

YangHighlighter::YangHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc)
{
    updateColors(false);
}

void YangHighlighter::updateColors(bool dark)
{
    m_rules.clear();
    auto fmt = [](const QColor &c, bool bold = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)
            f.setFontWeight(QFont::Bold);
        return f;
    };
    const QColor structural = dark ? QColor(0x7d, 0xb4, 0xff) : QColor(0x1d, 0x4e, 0xd8);
    const QColor statement  = dark ? QColor(0xc4, 0xa7, 0xff) : QColor(0x6d, 0x28, 0xd9);
    const QColor builtin    = dark ? QColor(0x5e, 0xea, 0xd4) : QColor(0x0f, 0x76, 0x6e);
    const QColor treeAccess = dark ? QColor(0xfb, 0xbf, 0x24) : QColor(0xb4, 0x53, 0x09);

    m_rules.push_back({QRegularExpression(wordAlternation(kStructural, std::size(kStructural))),
                       fmt(structural, true)});
    m_rules.push_back({QRegularExpression(wordAlternation(kStatements, std::size(kStatements))),
                       fmt(statement)});
    m_rules.push_back({QRegularExpression(wordAlternation(kBuiltinTypes, std::size(kBuiltinTypes))),
                       fmt(builtin)});
    // tree output: +--rw, +--ro, +---x, +---n, +---w
    m_rules.push_back({QRegularExpression(QStringLiteral("\\+--(rw|ro|-x|-n|-w)")),
                       fmt(treeAccess, true)});
    m_comment = fmt(dark ? QColor(0x8b, 0x95, 0xa5) : QColor(0x6b, 0x72, 0x80));
    m_comment.setFontItalic(true);
    m_string = fmt(dark ? QColor(0xfd, 0xa4, 0xaf) : QColor(0xb9, 0x1c, 0x1c));
    rehighlight();
}

void YangHighlighter::highlightBlock(const QString &text)
{
    for (const Rule &r : std::as_const(m_rules)) {
        auto it = r.re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(int(m.capturedStart()), int(m.capturedLength()), r.fmt);
        }
    }

    // Strings and comments: a small state machine so that "//" inside a
    // string is not taken as a comment. Block state 1 = inside /* */,
    // 2 = inside a double-quoted string, 3 = inside a single-quoted string
    // (YANG strings may span lines).
    int state = previousBlockState();
    int start = 0;
    int i = 0;
    const int n = int(text.size());
    if (state == 1 || state == 2 || state == 3)
        start = 0;
    else
        state = 0;

    while (i < n) {
        const QChar c = text.at(i);
        if (state == 0) {
            if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('/')) {
                setFormat(i, n - i, m_comment);
                setCurrentBlockState(0);
                return;
            }
            if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('*')) {
                state = 1;
                start = i;
                i += 2;
                continue;
            }
            if (c == QLatin1Char('"')) { state = 2; start = i; ++i; continue; }
            if (c == QLatin1Char('\'')) { state = 3; start = i; ++i; continue; }
            ++i;
        } else if (state == 1) {
            if (c == QLatin1Char('*') && i + 1 < n && text.at(i + 1) == QLatin1Char('/')) {
                setFormat(start, i + 2 - start, m_comment);
                state = 0;
                i += 2;
                continue;
            }
            ++i;
        } else if (state == 2) {
            if (c == QLatin1Char('\\')) { i += 2; continue; }
            if (c == QLatin1Char('"')) {
                setFormat(start, i + 1 - start, m_string);
                state = 0;
            }
            ++i;
        } else { // state 3
            if (c == QLatin1Char('\'')) {
                setFormat(start, i + 1 - start, m_string);
                state = 0;
            }
            ++i;
        }
    }
    if (state == 1)
        setFormat(start, n - start, m_comment);
    else if (state == 2 || state == 3)
        setFormat(start, n - start, m_string);
    setCurrentBlockState(state);
}

} // namespace ys
