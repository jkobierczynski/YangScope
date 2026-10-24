// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/XmlHighlighter.h"

namespace ys {

XmlHighlighter::XmlHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc)
{
    updateColors(false);
}

void XmlHighlighter::updateColors(bool dark)
{
    m_tag.setForeground(dark ? QColor(0x7d, 0xb4, 0xff) : QColor(0x1d, 0x4e, 0xd8));
    m_attr.setForeground(dark ? QColor(0xc4, 0xa7, 0xff) : QColor(0x6d, 0x28, 0xd9));
    m_value.setForeground(dark ? QColor(0xfd, 0xa4, 0xaf) : QColor(0xb9, 0x1c, 0x1c));
    m_comment.setForeground(dark ? QColor(0x8b, 0x95, 0xa5) : QColor(0x6b, 0x72, 0x80));
    m_comment.setFontItalic(true);
    m_jsonKey.setForeground(m_tag.foreground());
    rehighlight();
}

void XmlHighlighter::highlightBlock(const QString &text)
{
    static const QRegularExpression tag(QStringLiteral("</?[A-Za-z_][\\w:.\\-]*|/?>"));
    static const QRegularExpression attr(QStringLiteral("\\s([A-Za-z_][\\w:.\\-]*)\\s*="));
    static const QRegularExpression value(QStringLiteral("\"[^\"]*\"|'[^']*'"));
    static const QRegularExpression jsonKey(QStringLiteral("\"[^\"]*\"(?=\\s*:)"));
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"));

    const bool json = text.trimmed().startsWith(QLatin1Char('"'))
                      || text.trimmed().startsWith(QLatin1Char('{'))
                      || text.trimmed().startsWith(QLatin1Char('}'));
    auto apply = [&](const QRegularExpression &re, const QTextCharFormat &f, int group = 0) {
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(int(m.capturedStart(group)), int(m.capturedLength(group)), f);
        }
    };
    if (json) {
        apply(value, m_value);
        apply(jsonKey, m_jsonKey);
        return;
    }
    apply(tag, m_tag);
    apply(attr, m_attr, 1);
    // values only inside tags
    static const QRegularExpression inTag(QStringLiteral("<[^>]*>"));
    auto it = inTag.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        auto vit = value.globalMatch(m.captured(0));
        while (vit.hasNext()) {
            const auto v = vit.next();
            setFormat(int(m.capturedStart() + v.capturedStart()), int(v.capturedLength()), m_value);
        }
    }
    apply(comment, m_comment);
}

} // namespace ys
