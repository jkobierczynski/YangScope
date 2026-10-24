// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/DataTree.h"
#include "core/GnmiData.h"
#include "core/GnmiTypes.h"
#include "gui/DataTreeWidget.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPointF>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QCompleter;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QStringListModel;
class QTabWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
struct lysc_node;
struct lyd_node;

namespace ys {

class GnmiConnection;
class LiveGraph;
class YangContext;
class XmlHighlighter;

/// gNMI target panel: connection, Get, Set (validated locally with a change
/// preview) and Subscribe with a live table and graph.
class GnmiPanel : public QWidget {
    Q_OBJECT
public:
    enum class Op { Get, Set, Subscribe };

    GnmiPanel(const YangContext &ctx, QWidget *parent = nullptr);
    ~GnmiPanel() override;

    bool isConnected() const { return m_connected; }
    QString targetLabel() const { return m_targetLabel; }
    gnmi::Capabilities capabilities() const { return m_caps; }

    void setSelectedNode(const lysc_node *node);
    /// Prepare the request for @p node; Get and Subscribe start right away
    /// when @p run is set.
    void prepareFor(const lysc_node *node, Op op, bool run);

    void contextAboutToReset();
    void contextChanged();

    void connectDialog();
    void connectTo(const gnmi::ConnectParams &params, const QString &profile);
    void disconnectTarget();
    void applyThemeColors(bool dark);

    // for automation and tests
    void startGet();
    void startSubscription();
    void stopSubscription();

signals:
    void schemaNodeActivated(const lysc_node *node);
    void loadModelsRequested(const QVector<ys::gnmi::ModelData> &models, const QString &target);
    void logLines(const QStringList &lines);
    void statusMessage(const QString &text);
    void connectionChanged(bool connected);
    void copyRequested(const QString &text, const QString &what);
    void subscriptionSynced();

private:
    void buildUi();
    void updateUi();
    void setEditorText(QPlainTextEdit *e, const QString &text);
    void setLineText(QLineEdit *e, const QString &text);
    void showError(const QString &html);
    void addHistory(const QString &label, const QString &result, qint64 ms, bool ok);

    // Get
    void onGetFinished(int id, bool ok, const QVector<gnmi::Notification> &notes, qint64 ms,
                       const QString &error);
    void showNotifications(const QVector<gnmi::Notification> &notes);
    void rebuildDataTree();

    // Set
    bool parseScript(QVector<gnmi::SetOperation> *ops);
    void validateLocally(bool thenSend);
    void fetchNextConfig();
    void finishValidation();
    void onSetFinished(int id, bool ok, const gnmi::SetResult &res, qint64 ms, const QString &error);

    // Subscribe
    void onUpdate(const gnmi::Notification &n);
    void onSynced();
    void onSubscriptionFinished(bool ok, const QString &error);
    void showGraphFor(const QString &key);

    void onEntryAction(const lyd_node *node, DataTreeWidget::EntryAction action);
    void onOpened(const gnmi::Capabilities &caps);
    void onOpenFailed(const QString &error);
    void onClosed();
    void rebuildCompletions();

    struct Series {
        QTreeWidgetItem *item = nullptr;
        QVector<QPointF> values;   ///< (s, value)
        double lastValue = 0;
        double lastT = -1;
        double rate = 0;
        bool numeric = true;
        bool monotonic = true;
        int count = 0;
    };

    enum class Pending { None, Get, ValidateFetch, SendFetch, Set };

    const YangContext &m_ctx;
    GnmiConnection *m_conn = nullptr;
    bool m_connected = false;
    bool m_busy = false;        ///< a unary call is running
    bool m_subscribed = false;  ///< a subscription is running
    QString m_targetLabel;
    gnmi::Capabilities m_caps;
    const lysc_node *m_selected = nullptr;
    int m_nextId = 1;
    int m_pendingId = 0;
    Pending m_pending = Pending::None;
    QString m_pendingLabel;

    QVector<gnmi::Notification> m_lastNotes;
    DataTree m_data;

    // Set validation state
    QVector<gnmi::SetOperation> m_setOps;
    QStringList m_fetchQueue;
    QVector<gnmi::Notification> m_fetched;
    bool m_sendAfterValidation = false;

    // Subscribe state
    QHash<QString, Series> m_series;
    QString m_graphKey;
    QElapsedTimer m_clock;
    int m_syncs = 0;
    bool m_settingText = false;
    bool m_getEdited = false, m_setEdited = false, m_subEdited = false;

    // widgets
    QLabel *m_status = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QPushButton *m_disconnectBtn = nullptr;
    QPushButton *m_modelsBtn = nullptr;
    QComboBox *m_op = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_selectionLabel = nullptr;
    // get
    QLineEdit *m_getPath = nullptr;
    QComboBox *m_getType = nullptr;
    QComboBox *m_getEncoding = nullptr;
    QPushButton *m_getBtn = nullptr;
    // set
    QPlainTextEdit *m_setEditor = nullptr;
    QPushButton *m_validateBtn = nullptr;
    QPushButton *m_setBtn = nullptr;
    // subscribe
    QPlainTextEdit *m_subPaths = nullptr;
    QComboBox *m_listMode = nullptr;
    QComboBox *m_subMode = nullptr;
    QDoubleSpinBox *m_interval = nullptr;
    QCheckBox *m_suppress = nullptr;
    QCheckBox *m_updatesOnly = nullptr;
    QComboBox *m_subEncoding = nullptr;
    QPushButton *m_subscribeBtn = nullptr;
    QPushButton *m_pollBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    // results
    QLabel *m_errorBanner = nullptr;
    QTabWidget *m_tabs = nullptr;
    DataTreeWidget *m_dataTree = nullptr;
    QTreeWidget *m_updates = nullptr;
    QPlainTextEdit *m_json = nullptr;
    XmlHighlighter *m_jsonHl = nullptr;
    QTextBrowser *m_validation = nullptr;
    QWidget *m_livePage = nullptr;
    QTreeWidget *m_live = nullptr;
    LiveGraph *m_graph = nullptr;
    QLabel *m_liveStatus = nullptr;
    QTreeWidget *m_models = nullptr;
    QLabel *m_capsSummary = nullptr;
    QTreeWidget *m_history = nullptr;
    QCompleter *m_completer = nullptr;
    QStringListModel *m_completions = nullptr;
};

} // namespace ys
