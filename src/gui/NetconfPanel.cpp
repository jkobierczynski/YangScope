// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/NetconfPanel.h"

#include "core/NetconfXml.h"
#include "core/NodeInfo.h"
#include "core/YangContext.h"
#include "gui/ConnectDialog.h"
#include "gui/NetconfWorker.h"
#include "gui/XmlHighlighter.h"

#include <libyang/libyang.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QXmlStreamReader>

namespace ys {

using namespace ys::netconf;

namespace {
constexpr int kRequestRole = Qt::UserRole + 1;
constexpr int kRawRole = Qt::UserRole + 2;
constexpr int kMaxHistory = 200;
constexpr int kMaxListedChanges = 200;

QString deviceCacheDir(const QString &host, int port)
{
    QString safe = host;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._\\-]")), QStringLiteral("_"));
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/device-schemas/") + safe + QLatin1Char('_') + QString::number(port);
}

/// Accept either an operation or a full <rpc> element in the editor.
QByteArray operationFromEditor(const QString &text, QString *error)
{
    static const QRegularExpression decl(QStringLiteral("^\\s*<\\?xml[^>]*\\?>\\s*"));
    QString s = text.trimmed();
    s.remove(decl);
    // Well-formedness check (wrap so that several siblings are allowed)
    QXmlStreamReader r(QStringLiteral("<w>") + s + QStringLiteral("</w>"));
    QString rootName;
    int depth = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (++depth == 2 && rootName.isEmpty())
                rootName = r.name().toString();
        } else if (r.isEndElement()) {
            --depth;
        }
    }
    if (r.hasError()) {
        *error = QStringLiteral("request is not well-formed XML: %1 (line %2)")
                     .arg(r.errorString()).arg(r.lineNumber());
        return {};
    }
    if (rootName.isEmpty()) {
        *error = QStringLiteral("request is empty");
        return {};
    }
    if (rootName == QLatin1String("rpc")) {
        // strip the <rpc ...> wrapper, we add our own with a fresh message-id
        static const QRegularExpression wrap(QStringLiteral("^\\s*<rpc\\b[^>]*>(.*)</rpc>\\s*$"),
                                             QRegularExpression::DotMatchesEverythingOption);
        const auto m = wrap.match(s);
        if (m.hasMatch())
            s = m.captured(1);
    }
    return s.toUtf8();
}

/// Module name -> namespace for every module on the node's path.
QVector<QPair<QString, QString>> pathPrefixes(const lysc_node *node)
{
    QVector<QPair<QString, QString>> out;
    for (const lysc_node *n = node; n; n = n->parent) {
        const QString name = QString::fromUtf8(n->module->name);
        bool seen = false;
        for (const auto &p : out)
            seen |= p.first == name;
        if (!seen)
            out.prepend({name, QString::fromUtf8(n->module->ns)});
    }
    return out;
}

QString changeLines(const QVector<EditChange> &changes)
{
    QStringList lines;
    for (int i = 0; i < changes.size() && i < kMaxListedChanges; ++i)
        lines << changes[i].toString();
    if (changes.size() > kMaxListedChanges)
        lines << QStringLiteral("... and %1 more").arg(changes.size() - kMaxListedChanges);
    return lines.join(QLatin1Char('\n'));
}
} // namespace

NetconfPanel::NetconfPanel(const YangContext &ctx, QWidget *parent)
    : QWidget(parent), m_ctx(ctx), m_client(new NetconfClient(this))
{
    buildUi();
    NetconfWorker *w = m_client->worker();
    connect(w, &NetconfWorker::opened, this, &NetconfPanel::onOpened);
    connect(w, &NetconfWorker::openFailed, this, &NetconfPanel::onOpenFailed);
    connect(w, &NetconfWorker::rpcFinished, this, &NetconfPanel::onRpcFinished);
    connect(w, &NetconfWorker::closed, this, &NetconfPanel::onClosed);
    connect(w, &NetconfWorker::schemasSynced, this, &NetconfPanel::onSchemasSynced);
    connect(w, &NetconfWorker::schemaProgress, this, [this](int done, int total, const QString &cur) {
        if (!m_progress)
            return;
        m_progress->setMaximum(qMax(1, total));
        m_progress->setValue(done);
        m_progress->setLabelText(cur.isEmpty() ? tr("Finishing...") : tr("Downloading %1").arg(cur));
    });
    m_confirmTimer = new QTimer(this);
    m_confirmTimer->setInterval(1000);
    connect(m_confirmTimer, &QTimer::timeout, this, &NetconfPanel::updateConfirmCountdown);
    updateUi();
    rebuildRequest();
}

NetconfPanel::~NetconfPanel() = default;

void NetconfPanel::buildUi()
{
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // --- connection bar
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_connectBtn = new QPushButton(tr("Connect..."), this);
    m_disconnectBtn = new QPushButton(tr("Disconnect"), this);
    m_schemasBtn = new QPushButton(tr("Load Device Schemas"), this);
    m_schemasBtn->setToolTip(tr("Download the device's YANG modules (get-schema) and load "
                                "exactly what it implements, with its features"));
    auto *bar = new QHBoxLayout;
    bar->addWidget(m_status, 1);
    bar->addWidget(m_schemasBtn);
    bar->addWidget(m_connectBtn);
    bar->addWidget(m_disconnectBtn);

    // --- candidate datastore bar
    m_candidateBar = new QWidget(this);
    m_candidateState = new QLabel(this);
    m_lockBtn = new QPushButton(tr("Lock"), this);
    m_lockBtn->setToolTip(tr("Lock the candidate datastore so no other session can change it"));
    m_unlockBtn = new QPushButton(tr("Unlock"), this);
    m_serverValidateBtn = new QPushButton(tr("Validate"), this);
    m_serverValidateBtn->setToolTip(tr("Ask the device to validate the candidate (<validate>)"));
    m_commitBtn = new QPushButton(tr("Commit..."), this);
    m_commitBtn->setToolTip(tr("Show what changes and commit the candidate to running"));
    m_confirmedBtn = new QPushButton(tr("Confirmed Commit..."), this);
    m_confirmedBtn->setToolTip(tr("Commit with automatic rollback unless confirmed in time "
                                  "(a safety net when the change might cut you off)"));
    m_confirmTimeout = new QSpinBox(this);
    m_confirmTimeout->setRange(10, 3600);
    m_confirmTimeout->setValue(120);
    m_confirmTimeout->setSuffix(tr(" s"));
    m_cancelCommitBtn = new QPushButton(tr("Cancel Commit"), this);
    m_cancelCommitBtn->setToolTip(tr("Roll back the pending confirmed commit now"));
    m_discardBtn = new QPushButton(tr("Discard Changes..."), this);
    auto *cl = new QHBoxLayout(m_candidateBar);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(new QLabel(tr("<b>Candidate:</b>"), this));
    cl->addWidget(m_candidateState, 1);
    cl->addWidget(m_lockBtn);
    cl->addWidget(m_unlockBtn);
    cl->addWidget(m_serverValidateBtn);
    cl->addWidget(m_discardBtn);
    cl->addWidget(m_commitBtn);
    cl->addWidget(m_confirmedBtn);
    cl->addWidget(m_confirmTimeout);
    cl->addWidget(m_cancelCommitBtn);

    // --- request side
    m_op = new QComboBox(this);
    m_op->addItem(QStringLiteral("get"), int(Op::Get));
    m_op->addItem(QStringLiteral("get-config"), int(Op::GetConfig));
    m_op->addItem(QStringLiteral("edit-config"), int(Op::EditConfig));
    m_op->addItem(tr("custom RPC"), int(Op::Custom));
    m_datastore = new QComboBox(this);
    m_datastore->addItem(QStringLiteral("running"));
    m_datastore->setToolTip(tr("Datastore to read (get-config) or edit (edit-config)"));
    m_filterMode = new QComboBox(this);
    m_filterMode->addItem(tr("subtree filter"), QStringLiteral("subtree"));
    m_filterMode->addItem(tr("XPath filter"), QStringLiteral("xpath"));
    m_filterMode->addItem(tr("no filter"), QStringLiteral("none"));
    auto *opts = new QHBoxLayout;
    opts->addWidget(m_op);
    opts->addWidget(m_datastore);
    opts->addWidget(m_filterMode);
    opts->addStretch();

    m_editOptions = new QWidget(this);
    m_defaultOp = new QComboBox(this);
    m_defaultOp->addItem(tr("default-operation merge"), QStringLiteral("merge"));
    m_defaultOp->addItem(tr("default-operation replace"), QStringLiteral("replace"));
    m_defaultOp->addItem(tr("default-operation none"), QStringLiteral("none"));
    m_testOption = new QComboBox(this);
    m_testOption->addItem(tr("test: device default"), int(TestOption::Default));
    m_testOption->addItem(QStringLiteral("test-then-set"), int(TestOption::TestThenSet));
    m_testOption->addItem(QStringLiteral("set"), int(TestOption::Set));
    m_testOption->addItem(QStringLiteral("test-only"), int(TestOption::TestOnly));
    m_errorOption = new QComboBox(this);
    m_errorOption->addItem(tr("errors: device default"), int(ErrorOption::Default));
    m_errorOption->addItem(QStringLiteral("stop-on-error"), int(ErrorOption::StopOnError));
    m_errorOption->addItem(QStringLiteral("continue-on-error"), int(ErrorOption::ContinueOnError));
    m_errorOption->addItem(QStringLiteral("rollback-on-error"), int(ErrorOption::RollbackOnError));
    auto *eo = new QHBoxLayout(m_editOptions);
    eo->setContentsMargins(0, 0, 0, 0);
    eo->addWidget(m_defaultOp);
    eo->addWidget(m_testOption);
    eo->addWidget(m_errorOption);
    eo->addStretch();

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setForegroundRole(QPalette::PlaceholderText);
    m_selectionLabel->setTextFormat(Qt::PlainText);
    m_selectionLabel->setWordWrap(true);

    m_request = new QPlainTextEdit(this);
    m_request->setFont(mono);
    m_request->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_requestHl = new XmlHighlighter(m_request->document());

    m_rebuildBtn = new QPushButton(tr("Rebuild"), this);
    m_rebuildBtn->setToolTip(tr("Discard edits and rebuild the request from the selection"));
    m_validateBtn = new QPushButton(tr("Validate Locally"), this);
    m_validateBtn->setToolTip(tr("Check the edit with libyang against the loaded schemas and, when "
                                 "connected, against the device's current configuration"));
    m_sendBtn = new QPushButton(tr("Send"), this);
    m_sendBtn->setToolTip(tr("Send the request (Ctrl+Return)"));
    m_sendBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    auto *sendRow = new QHBoxLayout;
    sendRow->addWidget(m_rebuildBtn);
    sendRow->addStretch();
    sendRow->addWidget(m_cancelBtn);
    sendRow->addWidget(m_validateBtn);
    sendRow->addWidget(m_sendBtn);

    auto *left = new QWidget(this);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->addLayout(opts);
    ll->addWidget(m_editOptions);
    ll->addWidget(m_selectionLabel);
    ll->addWidget(m_request, 1);
    ll->addLayout(sendRow);

    // --- reply side
    m_errorBanner = new QLabel(this);
    m_errorBanner->setWordWrap(true);
    m_errorBanner->setTextFormat(Qt::RichText);
    m_errorBanner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorBanner->hide();

    m_dataTree = new DataTreeWidget(this);
    m_xml = new QPlainTextEdit(this);
    m_xml->setReadOnly(true);
    m_xml->setFont(mono);
    m_xml->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_xmlHl = new XmlHighlighter(m_xml->document());
    m_json = new QPlainTextEdit(this);
    m_json->setReadOnly(true);
    m_json->setFont(mono);
    m_json->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_jsonHl = new XmlHighlighter(m_json->document());
    m_validation = new QTextBrowser(this);
    m_validation->setPlaceholderText(tr("Choose edit-config and press Validate Locally."));
    m_capsList = new QTreeWidget(this);
    m_capsList->setHeaderLabels({tr("Capability"), tr("Module / name"), tr("Revision"), tr("Features")});
    m_capsList->setRootIsDecorated(false);
    m_capsList->setAlternatingRowColors(true);
    m_capsList->setSortingEnabled(true);
    m_capsList->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_capsList->setColumnWidth(0, 360);
    m_capsList->setColumnWidth(1, 200);
    m_history = new QTreeWidget(this);
    m_history->setHeaderLabels({tr("Time"), tr("Request"), tr("ms"), tr("Result")});
    m_history->setRootIsDecorated(false);
    m_history->setAlternatingRowColors(true);
    m_history->header()->setStretchLastSection(true);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(m_dataTree, tr("Data"));
    m_tabs->addTab(m_xml, tr("XML"));
    m_tabs->addTab(m_json, tr("JSON"));
    m_tabs->addTab(m_validation, tr("Validation"));
    m_tabs->addTab(m_capsList, tr("Capabilities"));
    m_tabs->addTab(m_history, tr("History"));

    auto *right = new QWidget(this);
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(m_errorBanner);
    rl->addWidget(m_tabs, 1);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addLayout(bar);
    lay->addWidget(m_candidateBar);
    lay->addWidget(split, 1);

    // object names for automation and tests
    m_validateBtn->setObjectName(QStringLiteral("validateBtn"));
    m_sendBtn->setObjectName(QStringLiteral("sendBtn"));
    m_commitBtn->setObjectName(QStringLiteral("commitBtn"));
    m_confirmedBtn->setObjectName(QStringLiteral("confirmedBtn"));
    m_cancelCommitBtn->setObjectName(QStringLiteral("cancelCommitBtn"));
    m_lockBtn->setObjectName(QStringLiteral("lockBtn"));
    m_unlockBtn->setObjectName(QStringLiteral("unlockBtn"));
    m_discardBtn->setObjectName(QStringLiteral("discardBtn"));
    m_request->setObjectName(QStringLiteral("requestEditor"));
    m_validation->setObjectName(QStringLiteral("validationView"));
    m_dataTree->setObjectName(QStringLiteral("dataTree"));
    m_candidateState->setObjectName(QStringLiteral("candidateState"));
    m_history->setObjectName(QStringLiteral("history"));
    m_errorBanner->setObjectName(QStringLiteral("errorBanner"));
    m_tabs->setObjectName(QStringLiteral("replyTabs"));

    connect(m_connectBtn, &QPushButton::clicked, this, &NetconfPanel::connectDialog);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &NetconfPanel::disconnectDevice);
    connect(m_schemasBtn, &QPushButton::clicked, this, &NetconfPanel::loadDeviceSchemas);
    connect(m_sendBtn, &QPushButton::clicked, this, &NetconfPanel::send);
    connect(m_validateBtn, &QPushButton::clicked, this, [this] { validateLocally(false); });
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] { m_client->cancel(); });
    connect(m_rebuildBtn, &QPushButton::clicked, this, [this] {
        m_requestEdited = false;
        rebuildRequest();
    });
    connect(m_lockBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::Lock); });
    connect(m_unlockBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::Unlock); });
    connect(m_serverValidateBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::Validate); });
    connect(m_commitBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::Commit); });
    connect(m_confirmedBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::ConfirmedCommit); });
    connect(m_cancelCommitBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::CancelCommit); });
    connect(m_discardBtn, &QPushButton::clicked, this, [this] { candidate(CandidateOp::Discard); });
    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_request);
    sc->setContext(Qt::WidgetShortcut);
    connect(sc, &QShortcut::activated, this, &NetconfPanel::send);
    for (QComboBox *c : {m_op, m_datastore, m_filterMode})
        connect(c, &QComboBox::currentIndexChanged, this, [this, c] {
            // switching the operation starts fresh; other options keep edits
            if (c == m_op)
                m_requestEdited = false;
            updateUi();
            rebuildRequest();
        });
    connect(m_request, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_settingRequest)
            m_requestEdited = true;
    });
    connect(m_dataTree, &DataTreeWidget::schemaNodeActivated, this, &NetconfPanel::schemaNodeActivated);
    connect(m_dataTree, &DataTreeWidget::copyRequested, this, &NetconfPanel::copyRequested);
    connect(m_dataTree, &DataTreeWidget::entryAction, this, &NetconfPanel::onEntryAction);
    connect(m_history, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        m_op->setCurrentIndex(m_op->findData(int(Op::Custom)));
        setRequestText(prettyXml(it->data(0, kRequestRole).toByteArray()), true);
        const QByteArray raw = it->data(0, kRawRole).toByteArray();
        if (!raw.isEmpty())
            showReply(parseReply(raw), raw, it->text(1), it->text(2).toLongLong());
    });
    applyThemeColors(false);
}

void NetconfPanel::applyThemeColors(bool dark)
{
    m_requestHl->updateColors(dark);
    m_xmlHl->updateColors(dark);
    m_jsonHl->updateColors(dark);
    m_errorBanner->setStyleSheet(dark
        ? QStringLiteral("QLabel { background: #450a0a; color: #fecaca; border: 1px solid #7f1d1d;"
                         " border-radius: 4px; padding: 6px; }")
        : QStringLiteral("QLabel { background: #fee2e2; color: #7f1d1d; border: 1px solid #fca5a5;"
                         " border-radius: 4px; padding: 6px; }"));
}

bool NetconfPanel::hasCapability(const QString &shortName) const
{
    for (const QString &c : m_caps)
        if (shortCapabilityName(c).startsWith(shortName))
            return true;
    return false;
}

void NetconfPanel::updateUi()
{
    if (m_connected) {
        m_status->setText(tr("<b style='color:#16a34a'>&#9679;</b> %1").arg(m_deviceLabel.toHtmlEscaped()));
    } else if (m_busy) {
        m_status->setText(tr("<b style='color:#d97706'>&#9679;</b> connecting..."));
    } else {
        m_status->setText(tr("<span style='color:gray'>&#9679; not connected</span>"));
    }
    m_connectBtn->setEnabled(!m_connected && !m_busy);
    m_disconnectBtn->setEnabled(m_connected || m_busy);
    m_schemasBtn->setEnabled(m_connected && !m_busy);
    m_cancelBtn->setVisible(m_busy);

    const Op op = Op(m_op->currentData().toInt());
    const bool edit = op == Op::EditConfig;
    m_datastore->setEnabled(op == Op::GetConfig || edit);
    m_filterMode->setVisible(!edit);
    m_filterMode->setEnabled(op != Op::Custom);
    m_editOptions->setVisible(edit);
    m_validateBtn->setVisible(edit);
    m_validateBtn->setEnabled(!m_busy);
    m_sendBtn->setText(edit ? tr("Validate && Send...") : tr("Send"));
    m_request->setPlaceholderText(edit
        ? tr("The children of <config>, e.g. <system xmlns=\"...\"><hostname>r1</hostname></system>.\n"
             "nc:operation=\"delete\" etc. can be used without declaring the nc prefix.\n"
             "Tip: right-click an entry in the Data tab and choose Edit This Entry.")
        : tr("<get>...</get>  (the <rpc> wrapper and message-id are added)"));

    // Offer only datastores the device advertises (all while disconnected,
    // so requests can be prepared offline).
    const bool cand = !m_connected || hasCapability(QStringLiteral(":candidate"));
    const bool writableRunning = !m_connected || hasCapability(QStringLiteral(":writable-running"));
    QStringList stores;
    if (edit) {
        if (cand)
            stores << QStringLiteral("candidate");
        if (writableRunning)
            stores << QStringLiteral("running");
    } else {
        stores << QStringLiteral("running");
        if (cand)
            stores << QStringLiteral("candidate");
        if (!m_connected || hasCapability(QStringLiteral(":startup")))
            stores << QStringLiteral("startup");
    }
    QStringList current;
    for (int i = 0; i < m_datastore->count(); ++i)
        current << m_datastore->itemText(i);
    if (current != stores) {
        const QString ds = m_datastore->currentText();
        const QSignalBlocker b(m_datastore);
        m_datastore->clear();
        m_datastore->addItems(stores);
        if (stores.contains(ds))
            m_datastore->setCurrentText(ds);
    }
    const bool canSend = m_connected && !m_busy && (!edit || !stores.isEmpty());
    m_sendBtn->setEnabled(canSend);
    m_sendBtn->setToolTip(edit && m_connected && stores.isEmpty()
                              ? tr("The device advertises neither :candidate nor :writable-running")
                              : tr("Send the request (Ctrl+Return)"));
    m_testOption->setEnabled(!m_connected || hasCapability(QStringLiteral(":validate")));
    m_errorOption->setItemData(3, (!m_connected || hasCapability(QStringLiteral(":rollback-on-error")))
                                      ? QVariant() : QVariant(tr("not advertised by the device")),
                               Qt::ToolTipRole);

    // candidate bar
    m_candidateBar->setVisible(m_connected && hasCapability(QStringLiteral(":candidate")));
    const bool idle = m_connected && !m_busy;
    const bool confirmedCap = hasCapability(QStringLiteral(":confirmed-commit"));
    m_lockBtn->setEnabled(idle && !m_candidateLocked);
    m_unlockBtn->setEnabled(idle && m_candidateLocked);
    m_serverValidateBtn->setVisible(hasCapability(QStringLiteral(":validate")));
    m_serverValidateBtn->setEnabled(idle);
    m_commitBtn->setEnabled(idle);
    m_commitBtn->setText(m_confirmPending ? tr("Confirm Commit") : tr("Commit..."));
    m_confirmedBtn->setVisible(confirmedCap);
    m_confirmTimeout->setVisible(confirmedCap);
    m_confirmedBtn->setEnabled(idle && !m_confirmPending);
    m_cancelCommitBtn->setVisible(confirmedCap);
    m_cancelCommitBtn->setEnabled(idle && m_confirmPending);
    m_discardBtn->setEnabled(idle);
    QStringList state;
    state << (m_candidateLocked ? tr("locked by this session") : tr("not locked"));
    if (m_candidateDirty)
        state << tr("<b>uncommitted changes</b>");
    if (m_confirmPending) {
        const qint64 left = qMax<qint64>(0, QDateTime::currentDateTime().secsTo(m_confirmDeadline));
        state << tr("<b style='color:#d97706'>confirmed commit pending: confirm within %1 s "
                    "or the device rolls back</b>").arg(left);
    }
    m_candidateState->setText(state.join(QStringLiteral(" &middot; ")));
}

QByteArray NetconfPanel::currentFilter() const
{
    const QString mode = m_filterMode->currentData().toString();
    if (!m_selected || mode == QLatin1String("none"))
        return {};
    if (mode == QLatin1String("xpath")) {
        const QString path = dataPath(m_selected);
        return path.isEmpty() ? QByteArray() : xpathFilter(path, pathPrefixes(m_selected));
    }
    return subtreeFilter(m_selected).toUtf8();
}

void NetconfPanel::setSelectedNode(const lysc_node *node)
{
    m_selected = node;
    if (node) {
        const QString p = dataPath(node);
        m_selectionLabel->setText(tr("From selection: %1").arg(p.isEmpty() ? QString::fromUtf8(node->name) : p));
    } else {
        m_selectionLabel->setText(tr("Select a node in the schema tree to filter or edit it."));
    }
    if (!m_requestEdited)
        rebuildRequest();
}

void NetconfPanel::setRequestText(const QString &text, bool edited)
{
    m_settingRequest = true;
    m_request->setPlainText(text);
    m_settingRequest = false;
    m_requestEdited = edited;
}

void NetconfPanel::rebuildRequest()
{
    if (m_requestEdited)
        return;
    const Op op = Op(m_op->currentData().toInt());
    QString text;
    switch (op) {
    case Op::Get:
        text = prettyXml(getOp(currentFilter()));
        break;
    case Op::GetConfig:
        text = prettyXml(getConfigOp(m_datastore->currentText(), currentFilter()));
        break;
    case Op::EditConfig:
        text = m_selected ? schemaSkeletonXml(m_selected) : QString();
        break;
    case Op::Custom:
        if (!m_request->toPlainText().trimmed().isEmpty())
            return; // keep what the user wrote
        text = prettyXml(QByteArrayLiteral("<get-schema xmlns=\"") + kMonitoringNs.toUtf8()
                         + QByteArrayLiteral("\"><identifier>ietf-interfaces</identifier></get-schema>"));
        break;
    }
    setRequestText(text, false);
}

void NetconfPanel::prepareFor(const lysc_node *node, Op op)
{
    m_op->setCurrentIndex(m_op->findData(int(op)));
    m_requestEdited = false;
    setSelectedNode(node);
    rebuildRequest();
    m_request->setFocus();
}

void NetconfPanel::sendFor(const lysc_node *node, Op op)
{
    m_op->setCurrentIndex(m_op->findData(int(op)));
    m_requestEdited = false;
    setSelectedNode(node);
    rebuildRequest();
    send();
}

bool NetconfPanel::sendRpc(const QByteArray &op, const QString &label, Purpose purpose)
{
    if (!m_connected || m_busy)
        return false;
    m_pendingRequest = m_nextRequest++;
    m_pendingOp = op;
    m_pendingPurpose = purpose;
    m_busy = true;
    updateUi();
    QMetaObject::invokeMethod(m_client->worker(), "rpc", Qt::QueuedConnection,
                              Q_ARG(int, m_pendingRequest), Q_ARG(QString, label),
                              Q_ARG(QByteArray, op));
    emit statusMessage(tr("Sent %1...").arg(label));
    return true;
}

void NetconfPanel::send()
{
    if (!m_connected || m_busy)
        return;
    if (Op(m_op->currentData().toInt()) == Op::EditConfig) {
        validateLocally(true);
        return;
    }
    QString err;
    const QByteArray op = operationFromEditor(m_request->toPlainText(), &err);
    if (op.isEmpty()) {
        showError(err.toHtmlEscaped());
        return;
    }
    m_errorBanner->hide();
    QString label = QString::fromUtf8(op).section(QLatin1Char('>'), 0, 0).mid(1).section(QLatin1Char(' '), 0, 0);
    if (m_selected && m_op->currentData().toInt() != int(Op::Custom) && !m_requestEdited)
        label += QLatin1Char(' ') + dataPath(m_selected);
    sendRpc(op, label, Purpose::User);
}

void NetconfPanel::showError(const QString &html)
{
    m_errorBanner->setText(html);
    m_errorBanner->show();
}

// --- edit-config --------------------------------------------------------------------

QString NetconfPanel::editTarget() const
{
    return m_datastore->currentText();
}

DefaultOperation NetconfPanel::defaultOperation() const
{
    const QString d = m_defaultOp->currentData().toString();
    if (d == QLatin1String("replace"))
        return DefaultOperation::Replace;
    if (d == QLatin1String("none"))
        return DefaultOperation::None;
    return DefaultOperation::Merge;
}

QByteArray NetconfPanel::editContent(QString *error) const
{
    QString s = m_request->toPlainText().trimmed();
    // tolerate a pasted <config> wrapper
    static const QRegularExpression wrap(QStringLiteral("^<config\\b[^>]*>(.*)</config>$"),
                                         QRegularExpression::DotMatchesEverythingOption);
    const auto m = wrap.match(s);
    if (m.hasMatch())
        s = m.captured(1).trimmed();
    if (s.isEmpty())
        *error = tr("The edit is empty. Select a node for a skeleton, or right-click an entry "
                    "in the Data tab and choose Edit This Entry.");
    return s.toUtf8();
}

void NetconfPanel::validateLocally(bool thenSend)
{
    QString err;
    const QByteArray content = editContent(&err);
    if (!err.isEmpty()) {
        showError(err.toHtmlEscaped());
        return;
    }
    if (thenSend && !m_connected)
        return;
    if (editTarget().isEmpty()) {
        showError(tr("No editable datastore: the device advertises neither :candidate nor :writable-running."));
        return;
    }
    m_errorBanner->hide();
    m_editToSend = content;
    if (!m_connected) {
        showValidation(validateEdit(m_ctx.raw(), content, nullptr, defaultOperation()));
        return;
    }
    // Full validation needs the target's current configuration.
    sendRpc(getConfigOp(editTarget()), tr("get-config %1 (for validation)").arg(editTarget()),
            thenSend ? Purpose::SendFetch : Purpose::ValidateFetch);
}

void NetconfPanel::finishValidation(const QByteArray &configData, bool thenSend)
{
    DataTree current;
    if (!current.parseXml(m_ctx.raw(), configData)) {
        QStringList msgs{tr("netconf: the current configuration could not be parsed")};
        for (const QString &m : current.messages())
            msgs << QStringLiteral("  ") + m;
        emit logLines(msgs);
    }
    const EditValidation v = validateEdit(m_ctx.raw(), m_editToSend, &current, defaultOperation());
    showValidation(v);
    if (!thenSend)
        return;

    const QString target = editTarget();
    bool go = false;
    if (!v.ok) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Edit Not Valid"));
        box.setText(tr("Local validation found %n problem(s). The device will most likely "
                       "reject this edit.", nullptr, int(v.errors.size())));
        box.setDetailedText(v.errors.join(QLatin1Char('\n')));
        QPushButton *anyway = box.addButton(tr("Send Anyway"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        go = box.clickedButton() == static_cast<QAbstractButton *>(anyway);
    } else {
        go = confirmChanges(tr("Send edit-config"),
                            tr("Send this edit to <b>%1</b>, datastore <b>%2</b>?")
                                .arg(m_deviceLabel.section(QLatin1Char(' '), 0, 0).toHtmlEscaped(), target),
                            v.changes, v.warnings, tr("Send"), v.changes.isEmpty());
    }
    if (go)
        sendEdit(m_editToSend);
}

void NetconfPanel::showValidation(const EditValidation &v)
{
    QString h;
    if (v.ok)
        h += tr("<p style='color:#16a34a'><b>&#10003; Valid</b> %1</p>")
                 .arg(v.fullValidation ? tr("against the loaded schemas and the device's current configuration")
                                       : tr("against the loaded schemas"));
    else
        h += tr("<p style='color:#dc2626'><b>&#10007; Not valid</b></p>");
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
        h += tr("<h4>Changes (%1)</h4>").arg(v.changes.size());
        if (v.changes.isEmpty()) {
            h += tr("<p>none</p>");
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
    if (v.operations)
        h += tr("<p style='color:gray'>%n explicit nc:operation attribute(s)</p>", nullptr, v.operations);
    m_validation->setHtml(h);
    m_tabs->setCurrentWidget(m_validation);
}

void NetconfPanel::sendEdit(const QByteArray &content)
{
    const QString def = m_defaultOp->currentData().toString();
    const QByteArray op = editConfigOp(editTarget(), content,
                                       def == QLatin1String("merge") ? QString() : def,
                                       TestOption(m_testOption->currentData().toInt()),
                                       ErrorOption(m_errorOption->currentData().toInt()));
    sendRpc(op, tr("edit-config %1").arg(editTarget()), Purpose::Edit);
}

bool NetconfPanel::confirmChanges(const QString &title, const QString &intro,
                                  const QVector<EditChange> &changes, const QStringList &warnings,
                                  const QString &okText, bool warnOnly)
{
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(760, 420);
    auto *lay = new QVBoxLayout(&dlg);
    auto *label = new QLabel(intro, &dlg);
    label->setWordWrap(true);
    lay->addWidget(label);
    QString summary;
    if (changes.isEmpty()) {
        summary = warnOnly ? tr("No changes compared with the current configuration.") : QString();
    } else {
        int c = 0, d = 0, r = 0;
        for (const EditChange &ch : changes) {
            c += ch.kind == EditChange::Create;
            d += ch.kind == EditChange::Delete;
            r += ch.kind == EditChange::Replace || ch.kind == EditChange::Move;
        }
        summary = tr("%1 added, %2 removed, %3 changed").arg(c).arg(d).arg(r);
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
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    return dlg.exec() == QDialog::Accepted;
}

void NetconfPanel::onEntryAction(const lyd_node *node, DataTreeWidget::EntryAction action)
{
    using A = DataTreeWidget::EntryAction;
    const lysc_node *sch = DataTree::schema(node);
    switch (action) {
    case A::FilterSubtree:
    case A::FilterXPath: {
        const Op cur = Op(m_op->currentData().toInt());
        const Op op = cur == Op::GetConfig ? Op::GetConfig : Op::Get;
        QByteArray filter;
        if (action == A::FilterXPath) {
            QVector<QPair<QString, QString>> pfx;
            const QString xp = entryXPath(node, &pfx);
            filter = xpathFilter(xp, pfx);
        } else {
            filter = entrySubtreeFilter(node);
        }
        {
            const QSignalBlocker b(m_op);
            m_op->setCurrentIndex(m_op->findData(int(op)));
        }
        updateUi();
        m_selected = sch;
        m_selectionLabel->setText(tr("Filter on entry: %1").arg(DataTree::path(node)));
        setRequestText(prettyXml(op == Op::Get ? getOp(filter)
                                               : getConfigOp(m_datastore->currentText(), filter)),
                       true);
        if (sch)
            emit schemaNodeActivated(sch);
        emit statusMessage(tr("Request filters on %1; press Send").arg(DataTree::path(node)));
        break;
    }
    case A::Edit:
    case A::Delete: {
        const QString xml = entryEditXml(node, action == A::Edit ? EntryXml::Config : EntryXml::Delete);
        if (xml.isEmpty()) {
            showError(tr("This entry has no configuration to edit."));
            return;
        }
        {
            const QSignalBlocker b(m_op);
            m_op->setCurrentIndex(m_op->findData(int(Op::EditConfig)));
        }
        updateUi();
        m_selected = sch;
        m_selectionLabel->setText(tr("%1: %2").arg(action == A::Edit ? tr("Edit") : tr("Delete"),
                                                  DataTree::path(node)));
        setRequestText(xml, true);
        m_request->setFocus();
        emit statusMessage(action == A::Edit ? tr("Change the values, then Validate & Send")
                                             : tr("Review the delete, then Validate & Send"));
        break;
    }
    }
}

// --- candidate datastore ---------------------------------------------------------------

void NetconfPanel::candidate(CandidateOp op)
{
    if (!m_connected || m_busy)
        return;
    const QString dev = m_deviceLabel.section(QLatin1Char(' '), 0, 0).toHtmlEscaped();
    switch (op) {
    case CandidateOp::Lock:
        m_pendingCandidateOp = op;
        sendRpc(lockOp(QStringLiteral("candidate")), QStringLiteral("lock candidate"), Purpose::Candidate);
        return;
    case CandidateOp::Unlock:
        m_pendingCandidateOp = op;
        sendRpc(unlockOp(QStringLiteral("candidate")), QStringLiteral("unlock candidate"), Purpose::Candidate);
        return;
    case CandidateOp::Validate:
        m_pendingCandidateOp = op;
        sendRpc(validateOp(QStringLiteral("candidate")), QStringLiteral("validate candidate"), Purpose::Candidate);
        return;
    case CandidateOp::Discard:
        if (QMessageBox::question(this, tr("Discard Changes"),
                                  tr("Discard all uncommitted changes in the candidate datastore of %1?").arg(dev))
            != QMessageBox::Yes)
            return;
        m_pendingCandidateOp = op;
        sendRpc(discardChangesOp(), QStringLiteral("discard-changes"), Purpose::Candidate);
        return;
    case CandidateOp::CancelCommit:
        if (QMessageBox::question(this, tr("Cancel Commit"),
                                  tr("Roll back the pending confirmed commit on %1 now?").arg(dev))
            != QMessageBox::Yes)
            return;
        m_pendingCandidateOp = op;
        sendRpc(cancelCommitOp(), QStringLiteral("cancel-commit"), Purpose::Candidate);
        return;
    case CandidateOp::Commit:
        if (m_confirmPending) {
            // a plain commit confirms a pending confirmed commit
            if (QMessageBox::question(this, tr("Confirm Commit"),
                                      tr("Make the pending commit on %1 permanent?").arg(dev))
                != QMessageBox::Yes)
                return;
            m_pendingCandidateOp = op;
            sendRpc(commitOp(), QStringLiteral("commit (confirm)"), Purpose::Candidate);
            return;
        }
        [[fallthrough]];
    case CandidateOp::ConfirmedCommit:
        // preview: fetch running and candidate, diff them, then ask
        m_pendingCandidateOp = op;
        sendRpc(getConfigOp(QStringLiteral("running")), tr("get-config running (commit preview)"),
                Purpose::CommitRunning);
        return;
    }
}

void NetconfPanel::finishCommitPreview()
{
    // m_commitRunning has running; m_lastReply holds candidate (set by caller)
    DataTree running, cand;
    running.parseXml(m_ctx.raw(), m_commitRunning);
    cand.parseXml(m_ctx.raw(), m_lastReply.data);
    QStringList warnings;
    const QVector<EditChange> changes = diffConfigs(m_ctx.raw(), running, cand, &warnings);
    const bool confirmed = m_pendingCandidateOp == CandidateOp::ConfirmedCommit;
    const QString dev = m_deviceLabel.section(QLatin1Char(' '), 0, 0).toHtmlEscaped();
    QString intro = confirmed
        ? tr("Commit the candidate to running on <b>%1</b> with automatic rollback after "
             "<b>%2 s</b> unless you confirm it?").arg(dev).arg(m_confirmTimeout->value())
        : tr("Commit the candidate to running on <b>%1</b>?").arg(dev);
    if (changes.isEmpty())
        intro += tr("<br>The candidate is the same as running.");
    if (!confirmChanges(confirmed ? tr("Confirmed Commit") : tr("Commit"), intro, changes, warnings,
                        confirmed ? tr("Commit with Rollback") : tr("Commit"), true))
        return;
    sendRpc(confirmed ? commitOp(m_confirmTimeout->value()) : commitOp(),
            confirmed ? tr("commit confirmed %1 s").arg(m_confirmTimeout->value()) : QStringLiteral("commit"),
            Purpose::Candidate);
}

void NetconfPanel::onCandidateReply(CandidateOp op, const Reply &reply)
{
    if (reply.hasErrors())
        return;
    switch (op) {
    case CandidateOp::Lock:
        m_candidateLocked = true;
        break;
    case CandidateOp::Unlock:
        m_candidateLocked = false;
        break;
    case CandidateOp::Validate:
        emit statusMessage(tr("The device accepts the candidate"));
        break;
    case CandidateOp::Discard:
        m_candidateDirty = false;
        break;
    case CandidateOp::Commit:
        m_candidateDirty = false;
        if (m_confirmPending) {
            m_confirmPending = false;
            m_confirmTimer->stop();
            emit logLines({tr("netconf: confirmed commit made permanent")});
        }
        break;
    case CandidateOp::ConfirmedCommit:
        m_candidateDirty = false;
        m_confirmPending = true;
        m_confirmDeadline = QDateTime::currentDateTime().addSecs(m_confirmTimeout->value());
        m_confirmTimer->start();
        emit logLines({tr("netconf: confirmed commit, rollback in %1 s unless confirmed")
                           .arg(m_confirmTimeout->value())});
        break;
    case CandidateOp::CancelCommit:
        m_confirmPending = false;
        m_confirmTimer->stop();
        emit logLines({tr("netconf: confirmed commit cancelled, the device rolled back")});
        break;
    }
}

void NetconfPanel::updateConfirmCountdown()
{
    if (m_confirmPending && QDateTime::currentDateTime() >= m_confirmDeadline) {
        m_confirmPending = false;
        m_confirmTimer->stop();
        showError(tr("<b>The confirmed commit was not confirmed in time.</b> The device has "
                     "rolled running back to the previous configuration."));
        emit logLines({tr("netconf: confirm timeout passed; the device rolled back")});
    }
    updateUi();
}

// --- replies ---------------------------------------------------------------------------

void NetconfPanel::onRpcFinished(int id, const QString &label, bool ok, const Reply &reply,
                                 const QByteArray &raw, qint64 ms, const QString &error)
{
    if (id != m_pendingRequest)
        return;
    m_busy = false;
    const QByteArray request = m_pendingOp;
    const Purpose purpose = m_pendingPurpose;
    if (!ok) {
        showError(tr("<b>%1 failed:</b> %2").arg(label.toHtmlEscaped(), error.toHtmlEscaped()));
        addHistory(label, request, raw, ms, error, false);
        emit logLines({QStringLiteral("netconf: %1 failed: %2").arg(label, error)});
        updateUi();
        return;
    }
    QString result;
    if (reply.hasErrors())
        result = reply.errors.first().toString();
    else if (reply.ok)
        result = QStringLiteral("ok");
    else if (reply.hasData)
        result = tr("data, %n byte(s)", nullptr, int(reply.data.size()));
    addHistory(label, request, raw, ms, result, !reply.hasErrors());
    updateUi();

    switch (purpose) {
    case Purpose::User:
        showReply(reply, raw, label, ms);
        break;
    case Purpose::ValidateFetch:
    case Purpose::SendFetch:
        if (reply.hasErrors()) {
            showReply(reply, raw, label, ms);
            return;
        }
        finishValidation(reply.data, purpose == Purpose::SendFetch);
        break;
    case Purpose::Edit:
        showReply(reply, raw, label, ms);
        if (!reply.hasErrors()) {
            if (editTarget() == QLatin1String("candidate"))
                m_candidateDirty = true;
            emit logLines({tr("netconf: %1 accepted").arg(label)});
            emit statusMessage(editTarget() == QLatin1String("candidate")
                                   ? tr("Edit accepted in candidate; Commit to apply it")
                                   : tr("Edit applied to %1").arg(editTarget()));
            updateUi();
        }
        break;
    case Purpose::CommitRunning:
        if (reply.hasErrors()) {
            showReply(reply, raw, label, ms);
            return;
        }
        m_commitRunning = reply.data;
        sendRpc(getConfigOp(QStringLiteral("candidate")), tr("get-config candidate (commit preview)"),
                Purpose::CommitCandidate);
        break;
    case Purpose::CommitCandidate:
        if (reply.hasErrors()) {
            showReply(reply, raw, label, ms);
            return;
        }
        m_lastReply = reply;
        finishCommitPreview();
        break;
    case Purpose::Candidate: {
        // the commit preview may have switched the pending op name; derive it
        CandidateOp op = m_pendingCandidateOp;
        showReply(reply, raw, label, ms);
        onCandidateReply(op, reply);
        updateUi();
        break;
    }
    }
}

void NetconfPanel::showReply(const Reply &reply, const QByteArray &raw, const QString &label, qint64 ms)
{
    m_lastReply = reply;
    m_lastRaw = raw;
    showRaw(raw);

    if (!reply.errors.isEmpty()) {
        QStringList lines;
        for (const RpcError &e : reply.errors)
            lines << e.toString().toHtmlEscaped();
        showError(tr("<b>%1</b> returned rpc-error:<br>%2").arg(label.toHtmlEscaped(),
                                                                lines.join(QStringLiteral("<br>"))));
        m_tabs->setCurrentWidget(m_xml);
    } else {
        m_errorBanner->hide();
    }
    // Replies without data (ok, rpc-error) leave the last data view in place.
    if (reply.hasData) {
        m_dataReply = reply;
        parseData();
    }
    if (!reply.hasErrors() && reply.hasData)
        m_tabs->setCurrentWidget(reply.data.isEmpty() && !reply.dataText.trimmed().isEmpty()
                                     ? static_cast<QWidget *>(m_xml) : m_dataTree);
    else if (reply.ok && m_tabs->currentWidget() != m_validation)
        m_tabs->setCurrentWidget(m_xml);
    emit statusMessage(tr("%1: %2 ms").arg(label).arg(ms));
}

void NetconfPanel::showRaw(const QByteArray &raw)
{
    QString text = prettyXml(raw);
    // get-schema returns the module text as character data; show it readably
    if (m_lastReply.hasData && m_lastReply.data.isEmpty() && !m_lastReply.dataText.trimmed().isEmpty())
        text = m_lastReply.dataText.trimmed();
    m_xml->setPlainText(text);
}

void NetconfPanel::parseData()
{
    m_dataTree->clearTree();
    m_json->clear();
    if (!m_dataReply.hasData || m_dataReply.data.isEmpty())
        return;
    const bool ok = m_data.parseXml(m_ctx.raw(), m_dataReply.data);
    QStringList msgs;
    for (const QString &m : m_data.messages())
        msgs << QStringLiteral("  ") + m;
    if (!ok) {
        emit logLines(QStringList{tr("netconf: reply data could not be parsed against the loaded schemas")} + msgs);
        m_json->setPlainText(tr("(reply could not be parsed against the loaded schemas; see Messages)"));
        return;
    }
    if (!msgs.isEmpty())
        emit logLines(msgs);
    m_dataTree->showTree(m_data);
    m_json->setPlainText(m_data.toJson());
}

void NetconfPanel::addHistory(const QString &label, const QByteArray &request, const QByteArray &raw,
                              qint64 ms, const QString &result, bool ok)
{
    auto *it = new QTreeWidgetItem({QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                    label, QString::number(ms), result});
    it->setData(0, kRequestRole, request);
    it->setData(0, kRawRole, raw);
    if (!ok)
        it->setForeground(3, QColor(0xdc, 0x26, 0x26));
    m_history->insertTopLevelItem(0, it);
    while (m_history->topLevelItemCount() > kMaxHistory)
        delete m_history->takeTopLevelItem(m_history->topLevelItemCount() - 1);
}

void NetconfPanel::contextAboutToReset()
{
    // The data tree references schema nodes of the old context.
    m_dataTree->clearTree();
    m_data = DataTree();
    m_selected = nullptr;
}

void NetconfPanel::contextChanged()
{
    m_selected = nullptr;
    setSelectedNode(nullptr);
    parseData();
}

// --- connection ------------------------------------------------------------------------

void NetconfPanel::connectDialog()
{
    ConnectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    connectTo(dlg.params(), dlg.profileName());
}

void NetconfPanel::connectTo(const ConnectParams &params, const QString &profile)
{
    if (m_connected || m_busy)
        return;
    if (!profile.isEmpty())
        QSettings().setValue(QStringLiteral("devices.last"), profile);
    m_host = params.host;
    m_port = params.port;
    m_deviceLabel = QStringLiteral("%1%2:%3")
                        .arg(params.user.isEmpty() ? QString() : params.user + QLatin1Char('@'),
                             params.host).arg(params.port);
    m_busy = true;
    m_errorBanner->hide();
    updateUi();
    emit logLines({tr("netconf: connecting to %1").arg(m_deviceLabel)});
    QMetaObject::invokeMethod(m_client->worker(), "open", Qt::QueuedConnection,
                              Q_ARG(ys::netconf::ConnectParams, params));
}

void NetconfPanel::onOpened(const QStringList &caps, const QString &sessionId, bool chunked,
                            const QString &fingerprint, const QString &warning)
{
    m_busy = false;
    m_connected = true;
    m_caps = caps;
    m_candidateLocked = m_candidateDirty = m_confirmPending = false;
    m_deviceLabel += tr("  session %1, NETCONF %2").arg(sessionId, chunked ? QStringLiteral("1.1")
                                                                            : QStringLiteral("1.0"));
    m_capsList->clear();
    for (const QString &c : caps) {
        const ModuleCapability m = parseModuleCapability(c);
        auto *it = new QTreeWidgetItem({c.section(QLatin1Char('?'), 0, 0),
                                        m.valid() ? m.module : shortCapabilityName(c),
                                        m.revision, m.features.join(QStringLiteral(", "))});
        it->setToolTip(0, c);
        m_capsList->addTopLevelItem(it);
    }
    m_capsList->sortByColumn(1, Qt::AscendingOrder);
    QStringList log{tr("netconf: connected to %1 (host key %2)").arg(m_deviceLabel, fingerprint),
                    tr("  %n capabilit(y/ies), %1 module(s)", nullptr, int(caps.size()))
                        .arg(moduleCapabilities(caps).size())};
    if (!warning.isEmpty())
        log << QStringLiteral("  ") + warning;
    emit logLines(log);
    updateUi();
    rebuildRequest();
    emit connectionChanged(true);
    if (m_ctx.implementedModules().isEmpty())
        emit statusMessage(tr("Connected. Use \"Load Device Schemas\" to browse what the device implements."));
}

void NetconfPanel::onOpenFailed(const QString &error)
{
    m_busy = false;
    m_connected = false;
    showError(tr("<b>Could not connect to %1:</b> %2").arg(m_deviceLabel.toHtmlEscaped(),
                                                           error.toHtmlEscaped()));
    emit logLines({tr("netconf: connect failed: %1").arg(error)});
    updateUi();
}

void NetconfPanel::disconnectDevice()
{
    if (m_busy)
        m_client->cancel();
    if (m_confirmPending)
        emit logLines({tr("netconf: disconnecting with a confirmed commit pending; the device "
                          "will roll back unless it is confirmed from another session")});
    QMetaObject::invokeMethod(m_client->worker(), "close", Qt::QueuedConnection);
}

void NetconfPanel::onClosed(const QString &reason)
{
    const bool was = m_connected;
    m_connected = false;
    m_busy = false;
    m_caps.clear();
    m_capsList->clear();
    m_candidateLocked = m_candidateDirty = false;
    // a pending confirmed commit keeps its countdown on the device; keep showing it
    if (!reason.isEmpty())
        showError(tr("<b>Session closed:</b> %1").arg(reason.toHtmlEscaped()));
    emit logLines({reason.isEmpty() ? tr("netconf: disconnected")
                                    : tr("netconf: session closed: %1").arg(reason)});
    updateUi();
    if (was)
        emit connectionChanged(false);
}

void NetconfPanel::loadDeviceSchemas()
{
    if (!m_connected || m_busy)
        return;
    m_busy = true;
    updateUi();
    m_progress = new QProgressDialog(tr("Reading the device's module list..."), tr("Cancel"), 0, 0, this);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(300);
    connect(m_progress, &QProgressDialog::canceled, this, [this] { m_client->cancel(); });
    QMetaObject::invokeMethod(m_client->worker(), "syncSchemas", Qt::QueuedConnection,
                              Q_ARG(QString, deviceCacheDir(m_host, m_port)));
}

void NetconfPanel::onSchemasSynced(const SchemaSyncResult &res)
{
    m_busy = false;
    if (m_progress) {
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    QStringList log{tr("netconf: %1 module(s) from %2; %3 schema file(s) downloaded, %4 cached, %5 failed")
                        .arg(res.modules.size()).arg(res.moduleSource)
                        .arg(res.downloaded.size()).arg(res.cached.size()).arg(res.failed.size()),
                    tr("  cache: %1").arg(QDir::toNativeSeparators(res.cacheDir))};
    for (const QString &m : res.messages)
        log << QStringLiteral("  ") + m;
    if (!res.canDownload)
        log << tr("  the device does not support ietf-netconf-monitoring (get-schema); "
                  "add its YANG files with File > Add Search Directory");
    emit logLines(log);
    updateUi();
    if (!res.modules.isEmpty())
        emit deviceSchemasReady(res, m_deviceLabel.section(QLatin1Char(' '), 0, 0));
}

} // namespace ys
