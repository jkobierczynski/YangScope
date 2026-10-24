// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/DataTree.h"
#include "core/NetconfSession.h"
#include "core/SchemaSync.h"

#include <QByteArray>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressDialog;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
struct lysc_node;

namespace ys {

class DataTreeWidget;
class NetconfClient;
class YangContext;
class XmlHighlighter;

/// NETCONF device panel: connection, request editor and reply views.
class NetconfPanel : public QWidget {
    Q_OBJECT
public:
    enum class Op { Get, GetConfig, Custom };

    NetconfPanel(const YangContext &ctx, QWidget *parent = nullptr);
    ~NetconfPanel() override;

    bool isConnected() const { return m_connected; }
    QString deviceLabel() const { return m_deviceLabel; }
    QStringList capabilities() const { return m_caps; }

    /// The schema tree selection; used to build filters.
    void setSelectedNode(const lysc_node *node);
    /// Build a request for @p node and send it right away.
    void sendFor(const lysc_node *node, Op op);

    /// Must be called before the schema context is destroyed or reset.
    void contextAboutToReset();
    /// The schema context changed: re-parse the shown reply against it.
    void contextChanged();

    void connectDialog();
    void connectTo(const netconf::ConnectParams &params, const QString &profile);
    void disconnectDevice();
    void loadDeviceSchemas();
    void applyThemeColors(bool dark);

signals:
    void schemaNodeActivated(const lysc_node *node);
    void deviceSchemasReady(const ys::netconf::SchemaSyncResult &result, const QString &device);
    void logLines(const QStringList &lines);
    void statusMessage(const QString &text);
    void connectionChanged(bool connected);
    void copyRequested(const QString &text, const QString &what);

private:
    void buildUi();
    void updateUi();
    void rebuildRequest();
    void send();
    void showReply(const netconf::Reply &reply, const QByteArray &raw, const QString &label,
                   qint64 ms);
    void showRaw(const QByteArray &raw);
    void parseData();
    void showError(const QString &html);
    void addHistory(const QString &label, const QByteArray &request, const QByteArray &raw,
                    qint64 ms, const QString &result, bool ok);

    void onOpened(const QStringList &caps, const QString &sessionId, bool chunked,
                  const QString &fingerprint, const QString &warning);
    void onOpenFailed(const QString &error);
    void onRpcFinished(int id, const QString &label, bool ok, const netconf::Reply &reply,
                       const QByteArray &raw, qint64 ms, const QString &error);
    void onClosed(const QString &reason);
    void onSchemasSynced(const netconf::SchemaSyncResult &res);

    bool hasCapability(const QString &shortName) const;
    QByteArray currentFilter() const;

    const YangContext &m_ctx;
    NetconfClient *m_client = nullptr;
    bool m_connected = false;
    bool m_busy = false;
    QString m_deviceLabel;
    QString m_host;
    int m_port = 0;
    QStringList m_caps;
    const lysc_node *m_selected = nullptr;
    int m_nextRequest = 1;
    int m_pendingRequest = 0;
    QByteArray m_pendingOp;
    bool m_requestEdited = false;
    bool m_settingRequest = false;

    netconf::Reply m_lastReply;
    QByteArray m_lastRaw;
    DataTree m_data;

    QLabel *m_status = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QPushButton *m_disconnectBtn = nullptr;
    QPushButton *m_schemasBtn = nullptr;
    QComboBox *m_op = nullptr;
    QComboBox *m_datastore = nullptr;
    QComboBox *m_filterMode = nullptr;
    QLabel *m_selectionLabel = nullptr;
    QPlainTextEdit *m_request = nullptr;
    XmlHighlighter *m_requestHl = nullptr;
    QPushButton *m_rebuildBtn = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QLabel *m_errorBanner = nullptr;
    QTabWidget *m_tabs = nullptr;
    DataTreeWidget *m_dataTree = nullptr;
    QPlainTextEdit *m_xml = nullptr;
    XmlHighlighter *m_xmlHl = nullptr;
    XmlHighlighter *m_jsonHl = nullptr;
    QPlainTextEdit *m_json = nullptr;
    QTreeWidget *m_capsList = nullptr;
    QTreeWidget *m_history = nullptr;
    QProgressDialog *m_progress = nullptr;
};

} // namespace ys
