// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/NetconfPanel.h"

#include "core/NetconfXml.h"
#include "core/NodeInfo.h"
#include "core/YangContext.h"
#include "gui/ConnectDialog.h"
#include "gui/DataTreeWidget.h"
#include "gui/NetconfWorker.h"
#include "gui/XmlHighlighter.h"

#include <libyang/libyang.h>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QXmlStreamReader>

namespace ys {

using namespace ys::netconf;

namespace {
constexpr int kRequestRole = Qt::UserRole + 1;
constexpr int kRawRole = Qt::UserRole + 2;
constexpr int kMaxHistory = 200;

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

    // --- request side
    m_op = new QComboBox(this);
    m_op->addItem(QStringLiteral("get"), int(Op::Get));
    m_op->addItem(QStringLiteral("get-config"), int(Op::GetConfig));
    m_op->addItem(tr("custom RPC"), int(Op::Custom));
    m_datastore = new QComboBox(this);
    m_datastore->addItem(QStringLiteral("running"));
    m_filterMode = new QComboBox(this);
    m_filterMode->addItem(tr("subtree filter"), QStringLiteral("subtree"));
    m_filterMode->addItem(tr("XPath filter"), QStringLiteral("xpath"));
    m_filterMode->addItem(tr("no filter"), QStringLiteral("none"));
    auto *opts = new QHBoxLayout;
    opts->addWidget(m_op);
    opts->addWidget(m_datastore);
    opts->addWidget(m_filterMode);
    opts->addStretch();

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setForegroundRole(QPalette::PlaceholderText);
    m_selectionLabel->setTextFormat(Qt::PlainText);
    m_selectionLabel->setWordWrap(true);

    m_request = new QPlainTextEdit(this);
    m_request->setFont(mono);
    m_request->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_request->setPlaceholderText(tr("<get>...</get>  (the <rpc> wrapper and message-id are added)"));
    m_requestHl = new XmlHighlighter(m_request->document());

    m_rebuildBtn = new QPushButton(tr("Rebuild"), this);
    m_rebuildBtn->setToolTip(tr("Discard edits and rebuild the request from the selection"));
    m_sendBtn = new QPushButton(tr("Send"), this);
    m_sendBtn->setToolTip(tr("Send the request (Ctrl+Return)"));
    m_sendBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    auto *sendRow = new QHBoxLayout;
    sendRow->addWidget(m_rebuildBtn);
    sendRow->addStretch();
    sendRow->addWidget(m_cancelBtn);
    sendRow->addWidget(m_sendBtn);

    auto *left = new QWidget(this);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->addLayout(opts);
    ll->addWidget(m_selectionLabel);
    ll->addWidget(m_request, 1);
    ll->addLayout(sendRow);

    // --- reply side
    m_errorBanner = new QLabel(this);
    m_errorBanner->setWordWrap(true);
    m_errorBanner->setTextFormat(Qt::RichText);
    m_errorBanner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorBanner->setStyleSheet(QStringLiteral(
        "QLabel { background: #fee2e2; color: #7f1d1d; border: 1px solid #fca5a5;"
        " border-radius: 4px; padding: 6px; }"));
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
    lay->addWidget(split, 1);

    connect(m_connectBtn, &QPushButton::clicked, this, &NetconfPanel::connectDialog);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &NetconfPanel::disconnectDevice);
    connect(m_schemasBtn, &QPushButton::clicked, this, &NetconfPanel::loadDeviceSchemas);
    connect(m_sendBtn, &QPushButton::clicked, this, &NetconfPanel::send);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] { m_client->cancel(); });
    connect(m_rebuildBtn, &QPushButton::clicked, this, [this] {
        m_requestEdited = false;
        rebuildRequest();
    });
    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_request);
    sc->setContext(Qt::WidgetShortcut);
    connect(sc, &QShortcut::activated, this, &NetconfPanel::send);
    for (QComboBox *c : {m_op, m_datastore, m_filterMode})
        connect(c, &QComboBox::currentIndexChanged, this, [this] {
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
    connect(m_history, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        m_settingRequest = true;
        m_request->setPlainText(prettyXml(it->data(0, kRequestRole).toByteArray()));
        m_settingRequest = false;
        m_requestEdited = true;
        const QByteArray raw = it->data(0, kRawRole).toByteArray();
        if (!raw.isEmpty()) {
            showReply(parseReply(raw), raw, it->text(1), it->text(2).toLongLong());
        }
    });
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
    m_sendBtn->setEnabled(m_connected && !m_busy);
    m_cancelBtn->setVisible(m_busy);

    const Op op = Op(m_op->currentData().toInt());
    m_datastore->setEnabled(op == Op::GetConfig);
    m_filterMode->setEnabled(op != Op::Custom);

    // Offer only datastores and filters the device advertises (all of them
    // while disconnected, so requests can be prepared offline).
    const QString ds = m_datastore->currentText();
    QStringList stores{QStringLiteral("running")};
    if (!m_connected || hasCapability(QStringLiteral(":candidate")))
        stores << QStringLiteral("candidate");
    if (!m_connected || hasCapability(QStringLiteral(":startup")))
        stores << QStringLiteral("startup");
    QStringList current;
    for (int i = 0; i < m_datastore->count(); ++i)
        current << m_datastore->itemText(i);
    if (current != stores) {
        const QSignalBlocker b(m_datastore);
        m_datastore->clear();
        m_datastore->addItems(stores);
        m_datastore->setCurrentText(stores.contains(ds) ? ds : QStringLiteral("running"));
    }
    const int xpathIdx = m_filterMode->findData(QStringLiteral("xpath"));
    m_filterMode->setItemData(xpathIdx,
                              (!m_connected || hasCapability(QStringLiteral(":xpath")))
                                  ? QVariant() : QVariant(tr("The device does not advertise :xpath")),
                              Qt::ToolTipRole);
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
        m_selectionLabel->setText(tr("Filter from selection: %1").arg(p.isEmpty() ? QString::fromUtf8(node->name) : p));
    } else {
        m_selectionLabel->setText(tr("Select a node in the schema tree to filter on it."));
    }
    if (!m_requestEdited)
        rebuildRequest();
}

void NetconfPanel::rebuildRequest()
{
    const Op op = Op(m_op->currentData().toInt());
    QByteArray xml;
    switch (op) {
    case Op::Get:
        xml = getOp(currentFilter());
        break;
    case Op::GetConfig:
        xml = getConfigOp(m_datastore->currentText(), currentFilter());
        break;
    case Op::Custom:
        if (m_request->toPlainText().trimmed().isEmpty())
            xml = QByteArrayLiteral("<get-schema xmlns=\"") + kMonitoringNs.toUtf8()
                  + QByteArrayLiteral("\"><identifier>ietf-interfaces</identifier></get-schema>");
        else
            return; // keep what the user wrote
        break;
    }
    m_settingRequest = true;
    m_request->setPlainText(prettyXml(xml));
    m_settingRequest = false;
    m_requestEdited = false;
}

void NetconfPanel::sendFor(const lysc_node *node, Op op)
{
    m_op->setCurrentIndex(m_op->findData(int(op)));
    setSelectedNode(node);
    m_requestEdited = false;
    rebuildRequest();
    send();
}

void NetconfPanel::send()
{
    if (!m_connected || m_busy)
        return;
    QString err;
    const QByteArray op = operationFromEditor(m_request->toPlainText(), &err);
    if (op.isEmpty()) {
        showError(err.toHtmlEscaped());
        return;
    }
    m_errorBanner->hide();
    const QString label = QString::fromUtf8(op).section(QLatin1Char('>'), 0, 0).mid(1)
                              .section(QLatin1Char(' '), 0, 0)
                          + (m_selected && m_op->currentData().toInt() != int(Op::Custom) && !m_requestEdited
                                 ? QStringLiteral(" ") + dataPath(m_selected) : QString());
    m_pendingRequest = m_nextRequest++;
    m_busy = true;
    updateUi();
    QMetaObject::invokeMethod(m_client->worker(), "rpc", Qt::QueuedConnection,
                              Q_ARG(int, m_pendingRequest), Q_ARG(QString, label),
                              Q_ARG(QByteArray, op));
    m_pendingOp = op;
    emit statusMessage(tr("Sent %1...").arg(label));
}

void NetconfPanel::showError(const QString &html)
{
    m_errorBanner->setText(html);
    m_errorBanner->show();
}

void NetconfPanel::onRpcFinished(int id, const QString &label, bool ok, const Reply &reply,
                                 const QByteArray &raw, qint64 ms, const QString &error)
{
    if (id != m_pendingRequest)
        return;
    m_busy = false;
    const QByteArray request = m_pendingOp;
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
    showReply(reply, raw, label, ms);
    updateUi();
}

void NetconfPanel::showReply(const Reply &reply, const QByteArray &raw, const QString &label, qint64 ms)
{
    m_lastReply = reply;
    m_lastRaw = raw;
    showRaw(raw);

    if (reply.hasErrors() || !reply.errors.isEmpty()) {
        QStringList lines;
        for (const RpcError &e : reply.errors)
            lines << e.toString().toHtmlEscaped();
        showError(tr("<b>%1</b> returned rpc-error:<br>%2").arg(label.toHtmlEscaped(),
                                                                lines.join(QStringLiteral("<br>"))));
        m_tabs->setCurrentWidget(m_xml);
    } else {
        m_errorBanner->hide();
    }
    parseData();
    if (!reply.hasErrors() && reply.hasData)
        m_tabs->setCurrentWidget(reply.data.isEmpty() && !reply.dataText.trimmed().isEmpty()
                                     ? static_cast<QWidget *>(m_xml) : m_dataTree);
    else if (reply.ok)
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
    if (!m_lastReply.hasData || m_lastReply.data.isEmpty())
        return;
    const bool ok = m_data.parseXml(m_ctx.raw(), m_lastReply.data);
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

// --- connection ----------------------------------------------------------------

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
    QMetaObject::invokeMethod(m_client->worker(), "close", Qt::QueuedConnection);
}

void NetconfPanel::onClosed(const QString &reason)
{
    const bool was = m_connected;
    m_connected = false;
    m_busy = false;
    m_caps.clear();
    m_capsList->clear();
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
