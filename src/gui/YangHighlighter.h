// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace ys {

/// Syntax highlighting for YANG source and libyang tree output.
class YangHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit YangHighlighter(QTextDocument *doc);
    void updateColors(bool dark);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression re;
        QTextCharFormat fmt;
    };
    QVector<Rule> m_rules;
    QTextCharFormat m_comment;
    QTextCharFormat m_string;
};

} // namespace ys
