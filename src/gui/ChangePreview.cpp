// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ChangePreview.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ys {

namespace {
constexpr int kMaxListedChanges = 200;
} // namespace

QString changeLines(const QVector<EditChange> &changes)
{
    QStringList lines;
    for (int i = 0; i < changes.size() && i < kMaxListedChanges; ++i)
        lines << changes[i].toString();
    if (changes.size() > kMaxListedChanges)
        lines << QStringLiteral("... and %1 more").arg(changes.size() - kMaxListedChanges);
    return lines.join(QLatin1Char('\n'));
}

QString validationHtml(const EditValidation &v, const QString &operationsLabel)
{
    QString h;
    if (v.ok)
        h += QObject::tr("<p style='color:#16a34a'><b>&#10003; Valid</b> %1</p>")
                 .arg(v.fullValidation ? QObject::tr("against the loaded schemas and the device's current configuration")
                                       : QObject::tr("against the loaded schemas"));
    else
        h += QObject::tr("<p style='color:#dc2626'><b>&#10007; Not valid</b></p>");
    if (!v.errors.isEmpty()) {
        h += QStringLiteral("<ul>");
        for (const QString &e : v.errors)
            h += QStringLiteral("<li style='color:#dc2626'>%1</li>").arg(e.toHtmlEscaped());
        h += QStringLiteral("</ul>");
    }
    if (!v.warnings.isEmpty()) {
        h += QStringLiteral("<ul>");
        for (const QString &w : v.warnings)
            h += QStringLiteral("<li style='color:#b45309'>%1</li>").arg(w.toHtmlEscaped());
        h += QStringLiteral("</ul>");
    }
    if (v.fullValidation) {
        h += QObject::tr("<h4>Changes (%1)</h4>").arg(v.changes.size());
        if (v.changes.isEmpty()) {
            h += QObject::tr("<p>none</p>");
        } else {
            h += QStringLiteral("<pre>");
            for (int i = 0; i < v.changes.size() && i < kMaxListedChanges; ++i) {
                const EditChange &c = v.changes[i];
                const char *color = c.kind == EditChange::Create ? "#16a34a"
                                  : c.kind == EditChange::Delete ? "#dc2626" : "#b45309";
                h += QStringLiteral("<span style='color:%1'>%2</span>\n")
                         .arg(QLatin1String(color), c.toString().toHtmlEscaped());
            }
            h += QStringLiteral("</pre>");
        }
    }
    if (v.operations && !operationsLabel.isEmpty())
        h += QStringLiteral("<p style='color:gray'>%1 %2</p>").arg(v.operations).arg(operationsLabel.toHtmlEscaped());
    return h;
}

bool confirmChangesDialog(QWidget *parent, const QString &title, const QString &intro,
                          const QVector<EditChange> &changes, const QStringList &warnings,
                          const QString &okText, bool warnOnly)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(760, 420);
    auto *lay = new QVBoxLayout(&dlg);
    auto *label = new QLabel(intro, &dlg);
    label->setWordWrap(true);
    lay->addWidget(label);
    QString summary;
    if (changes.isEmpty()) {
        summary = warnOnly ? QObject::tr("No changes compared with the current configuration.") : QString();
    } else {
        int c = 0, d = 0, r = 0;
        for (const EditChange &ch : changes) {
            c += ch.kind == EditChange::Create;
            d += ch.kind == EditChange::Delete;
            r += ch.kind == EditChange::Replace || ch.kind == EditChange::Move;
        }
        summary = QObject::tr("%1 added, %2 removed, %3 changed").arg(c).arg(d).arg(r);
    }
    if (!summary.isEmpty())
        lay->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(summary.toHtmlEscaped()), &dlg));
    auto *list = new QPlainTextEdit(&dlg);
    list->setReadOnly(true);
    list->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    list->setLineWrapMode(QPlainTextEdit::NoWrap);
    QString text = changeLines(changes);
    for (const QString &w : warnings)
        text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + QStringLiteral("note: ") + w;
    list->setPlainText(text);
    list->setVisible(!text.isEmpty());
    lay->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    QPushButton *ok = buttons->addButton(okText, QDialogButtonBox::AcceptRole);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    ok->setAutoDefault(false);
    lay->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    return dlg.exec() == QDialog::Accepted;
}

} // namespace ys
