// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/SearchPanel.h"

#include "core/NodeInfo.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace ys {

namespace {
constexpr int kMaxResults = 2000;
constexpr int kNodeRole = Qt::UserRole + 1;
} // namespace

SearchPanel::SearchPanel(const YangContext &ctx, QWidget *parent)
    : QWidget(parent), m_ctx(ctx)
{
    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(tr("Search nodes (name or path, e.g. counters or interface/state)"));
    m_edit->setClearButtonEnabled(true);

    m_inDescriptions = new QCheckBox(tr("Descriptions"), this);
    m_inDescriptions->setToolTip(tr("Also match the text of description statements"));
    m_regex = new QCheckBox(tr("Regex"), this);

    m_status = new QLabel(this);
    m_status->setForegroundRole(QPalette::PlaceholderText);

    m_results = new QTreeWidget(this);
    m_results->setColumnCount(3);
    m_results->setHeaderLabels({tr("Name"), tr("Kind"), tr("Path")});
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->setAlternatingRowColors(true);
    m_results->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_results->header()->setStretchLastSection(true);

    auto *opts = new QHBoxLayout;
    opts->addWidget(m_inDescriptions);
    opts->addWidget(m_regex);
    opts->addStretch();
    opts->addWidget(m_status);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addWidget(m_edit);
    lay->addLayout(opts);
    lay->addWidget(m_results);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &SearchPanel::runSearch);
    connect(m_edit, &QLineEdit::textChanged, m_debounce, qOverload<>(&QTimer::start));
    connect(m_edit, &QLineEdit::returnPressed, this, &SearchPanel::runSearch);
    connect(m_inDescriptions, &QCheckBox::toggled, this, &SearchPanel::runSearch);
    connect(m_regex, &QCheckBox::toggled, this, &SearchPanel::runSearch);
    connect(m_results, &QTreeWidget::itemActivated, this, &SearchPanel::onItemActivated);
    connect(m_results, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *cur) { if (cur) onItemActivated(cur); });
}

void SearchPanel::focusSearch()
{
    m_edit->setFocus();
    m_edit->selectAll();
}

void SearchPanel::setQuery(const QString &text)
{
    m_edit->setText(text);
    runSearch();
}

void SearchPanel::invalidate()
{
    m_results->clear();
    if (!m_edit->text().trimmed().isEmpty())
        runSearch();
    else
        m_status->clear();
}

void SearchPanel::runSearch()
{
    m_debounce->stop();
    m_results->clear();
    const QString needle = m_edit->text().trimmed();
    if (needle.isEmpty()) {
        m_status->clear();
        return;
    }

    QRegularExpression re;
    if (m_regex->isChecked()) {
        re = QRegularExpression(needle, QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            m_status->setText(tr("invalid regex"));
            return;
        }
    }
    // A needle with '/' is matched against the data path, otherwise the name.
    const bool pathMode = !m_regex->isChecked() && needle.contains(QLatin1Char('/'));
    const bool inDsc = m_inDescriptions->isChecked();

    auto matches = [&](const QString &s) {
        return m_regex->isChecked() ? re.match(s).hasMatch()
                                    : s.contains(needle, Qt::CaseInsensitive);
    };

    QList<QTreeWidgetItem *> items;
    int total = 0;
    std::function<void(const lysc_node *)> walk = [&](const lysc_node *n) {
        const QString name = QString::fromUtf8(n->name);
        bool hit = false;
        QString path;
        if (pathMode) {
            path = dataPath(n);
            hit = matches(path);
        } else {
            hit = matches(name) || (inDsc && n->dsc && matches(QString::fromUtf8(n->dsc)));
        }
        if (hit) {
            ++total;
            if (items.size() < kMaxResults) {
                if (path.isEmpty())
                    path = dataPath(n);
                auto *it = new QTreeWidgetItem({name, nodeKind(n), path});
                it->setData(0, kNodeRole, QVariant::fromValue(static_cast<const void *>(n)));
                it->setToolTip(2, path);
                items << it;
            }
        }
        for (const lysc_node *c : childrenOf(n))
            walk(c);
    };

    for (const lys_module *mod : m_ctx.implementedModules())
        for (const lysc_node *top : topLevelOf(mod))
            walk(top);

    m_results->addTopLevelItems(items);
    if (total > kMaxResults)
        m_status->setText(tr("%1 matches (showing first %2)").arg(total).arg(kMaxResults));
    else
        m_status->setText(tr("%n match(es)", nullptr, total));
}

void SearchPanel::onItemActivated(QTreeWidgetItem *item)
{
    if (!item)
        return;
    auto *n = static_cast<const lysc_node *>(item->data(0, kNodeRole).value<const void *>());
    if (n)
        emit nodeActivated(n);
}

} // namespace ys
