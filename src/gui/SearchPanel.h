// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
struct lysc_node;

namespace ys {

class YangContext;

/// Search across every node of every implemented module, by name, path or
/// description. Results are listed with their data path; activating one
/// emits nodeActivated so the main tree can jump to it.
class SearchPanel : public QWidget {
    Q_OBJECT
public:
    explicit SearchPanel(const YangContext &ctx, QWidget *parent = nullptr);

    void focusSearch();
    /// Set the search text and run the search immediately.
    void setQuery(const QString &text);
    /// Call after the context changed so results are refreshed.
    void invalidate();

signals:
    void nodeActivated(const lysc_node *node);

private:
    void runSearch();
    void onItemActivated(QTreeWidgetItem *item);

    const YangContext &m_ctx;
    QLineEdit *m_edit = nullptr;
    QCheckBox *m_inDescriptions = nullptr;
    QCheckBox *m_regex = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_results = nullptr;
    QTimer *m_debounce = nullptr;
};

} // namespace ys
