// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace ys {

/// Light XML highlighting for NETCONF requests and replies (also JSON keys).
class XmlHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit XmlHighlighter(QTextDocument *doc);
    void updateColors(bool dark);

protected:
    void highlightBlock(const QString &text) override;

private:
    QTextCharFormat m_tag, m_attr, m_value, m_comment, m_jsonKey;
};

} // namespace ys
