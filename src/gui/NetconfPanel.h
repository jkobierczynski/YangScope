// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/DataTree.h"
#include "core/EditConfig.h"
#include "core/NetconfSession.h"
#include "core/SchemaSync.h"
#include "gui/DataTreeWidget.h"

#include <QByteArray>
#include <QDateTime>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressDialog;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTextBrowser;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
struct lysc_node;
struct lyd_node;

namespace ys {

class NetconfClient;
class YangContext;
class XmlHighlighter;

/// NETCONF device panel: connection, request editor, edit-config with local
/// validation, the candidate datastore workflow and reply views.
class NetconfPanel : public QWidget {
    Q_OBJECT
public:
    enum class Op { Get, GetConfig, EditConfig, Custom };

    NetconfPanel(const YangContext &ctx, QWidget *parent = nullptr);
    ~NetconfPanel() override;

    bool isConnected() const { return m_connected; }
    QString deviceLabel() const { return m_deviceLabel; }
    QStringList capabilities() const { return m_caps; }

    /// The schema tree selection; used to build filters and edit skeletons.
    void setSelectedNode(const lysc_node *node);
    /// Build a request for @p node and send it right away.
    void sendFor(const lysc_node *node, Op op);
    /// Build a request for @p node in the editor without sending it.
    void prepareFor(const lysc_node *node, Op op);

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
    /// Why a request was sent; decides what happens with the reply.
    enum class Purpose {
        User,              ///< from the editor, shown as is
        ValidateFetch,     ///< current config for local validation
        SendFetch,         ///< current config, then validate, confirm and send
        Edit,              ///< the edit-config itself
        CommitRunning,     ///< running, for the commit preview
        CommitCandidate,   ///< candidate, for the commit preview
        Candidate          ///< lock, unlock, validate, commit, discard, cancel-commit
    };
    enum class CandidateOp { Lock, Unlock, Validate, Commit, ConfirmedCommit, CancelCommit, Discard };

    void buildUi();
    void updateUi();
    void rebuildRequest();
    void setRequestText(const QString &text, bool edited);
    void send();
    bool sendRpc(const QByteArray &op, const QString &label, Purpose purpose);
    void showReply(const netconf::Reply &reply, const QByteArray &raw, const QString &label,
                   qint64 ms);
    void showRaw(const QByteArray &raw);
    void parseData();
    void showError(const QString &html);
    void addHistory(const QString &label, const QByteArray &request, const QByteArray &raw,
                    qint64 ms, const QString &result, bool ok);

    // edit-config
    QString editTarget() const;
    DefaultOperation defaultOperation() const;
    QByteArray editContent(QString *error) const;
    void validateLocally(bool thenSend);
    void finishValidation(const QByteArray &configData, bool thenSend);
    void showValidation(const EditValidation &v);
    void sendEdit(const QByteArray &content);
    void onEntryAction(const lyd_node *node, DataTreeWidget::EntryAction action);

    // candidate workflow
    void candidate(CandidateOp op);
    void finishCommitPreview();
    void onCandidateReply(CandidateOp op, const netconf::Reply &reply);
    void updateConfirmCountdown();
    bool confirmChanges(const QString &title, const QString &intro,
                        const QVector<EditChange> &changes, const QStringList &warnings,
                        const QString &okText, bool warnOnly);

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
    Purpose m_pendingPurpose = Purpose::User;
    CandidateOp m_pendingCandidateOp = CandidateOp::Lock;
    bool m_requestEdited = false;
    bool m_settingRequest = false;

    // edit / candidate state
    QByteArray m_editToSend;        ///< validated content waiting for its config fetch
    QByteArray m_commitRunning;     ///< running config fetched for the commit preview
    bool m_candidateLocked = false;
    bool m_candidateDirty = false;
    bool m_confirmPending = false;
    QDateTime m_confirmDeadline;
    QTimer *m_confirmTimer = nullptr;

    netconf::Reply m_lastReply;
    netconf::Reply m_dataReply;     ///< last reply that carried <data>, shown in Data/JSON
    QByteArray m_lastRaw;
    DataTree m_data;

    QLabel *m_status = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QPushButton *m_disconnectBtn = nullptr;
    QPushButton *m_schemasBtn = nullptr;

    QWidget *m_candidateBar = nullptr;
    QLabel *m_candidateState = nullptr;
    QPushButton *m_lockBtn = nullptr;
    QPushButton *m_unlockBtn = nullptr;
    QPushButton *m_serverValidateBtn = nullptr;
    QPushButton *m_commitBtn = nullptr;
    QPushButton *m_confirmedBtn = nullptr;
    QSpinBox *m_confirmTimeout = nullptr;
    QPushButton *m_cancelCommitBtn = nullptr;
    QPushButton *m_discardBtn = nullptr;

    QComboBox *m_op = nullptr;
    QComboBox *m_datastore = nullptr;
    QComboBox *m_filterMode = nullptr;
    QWidget *m_editOptions = nullptr;
    QComboBox *m_defaultOp = nullptr;
    QComboBox *m_testOption = nullptr;
    QComboBox *m_errorOption = nullptr;
    QLabel *m_selectionLabel = nullptr;
    QPlainTextEdit *m_request = nullptr;
    XmlHighlighter *m_requestHl = nullptr;
    QPushButton *m_rebuildBtn = nullptr;
    QPushButton *m_validateBtn = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QLabel *m_errorBanner = nullptr;
    QTabWidget *m_tabs = nullptr;
    DataTreeWidget *m_dataTree = nullptr;
    QPlainTextEdit *m_xml = nullptr;
    XmlHighlighter *m_xmlHl = nullptr;
    XmlHighlighter *m_jsonHl = nullptr;
    QPlainTextEdit *m_json = nullptr;
    QTextBrowser *m_validation = nullptr;
    QTreeWidget *m_capsList = nullptr;
    QTreeWidget *m_history = nullptr;
    QProgressDialog *m_progress = nullptr;
};

} // namespace ys
