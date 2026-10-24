// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/GnmiPanel.h"

#include "core/NodeInfo.h"
#include "core/YangContext.h"
#include "gui/ChangePreview.h"
#include "gui/GnmiConnectDialog.h"
#include "gui/GnmiWorker.h"
#include "gui/LiveGraph.h"
#include "gui/XmlHighlighter.h"

#include <libyang/libyang.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringListModel>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace ys {

using namespace ys::gnmi;

namespace {
constexpr int kMaxHistory = 200;
constexpr int kMaxPoints = 600;
constexpr int kMaxUpdateRows = 20000;
constexpr int kMaxCompletions = 60000;

/// Display text of a value; JSON scalars without their quotes.
QString displayValue(const Value &v)
{
    if (v.isJson()) {
        const QByteArray t = v.json.trimmed();
        if (t.startsWith('"') && t.endsWith('"') && t.size() >= 2) {
            const QJsonDocument d = QJsonDocument::fromJson("[" + t + "]");
            if (d.isArray())
                return d.array().at(0).toString();
        }
        if (!t.startsWith('{') && !t.startsWith('['))
            return QString::fromUtf8(t);
    }
    return v.text;
}

bool toNumber(const QString &s, double *out)
{
    bool ok = false;
    const double d = s.toDouble(&ok);
    if (ok)
        *out = d;
    return ok;
}

QString lastName(const QString &key)
{
    QString s = key.section(QLatin1Char('/'), -1);
    s = s.section(QLatin1Char('['), 0, 0);
    return s.section(QLatin1Char(':'), -1);
}

QString skeletonFor(const lysc_node *n)
{
    const QString path = pathToString(pathForSchemaNode(n, false));
    QString s = QStringLiteral("# set the list key values marked * before sending\n");
    if (!path.contains(QLatin1Char('*')))
        s.clear();
    if (n->nodetype & (LYS_LEAF | LYS_LEAFLIST))
        return s + QStringLiteral("update %1 = ").arg(path);
    return s + QStringLiteral("update %1 = {\n  \n}").arg(path);
}
} // namespace

GnmiPanel::GnmiPanel(const YangContext &ctx, QWidget *parent)
    : QWidget(parent), m_ctx(ctx), m_conn(new GnmiConnection(this))
{
    buildUi();
    GnmiCallWorker *w = m_conn->calls();
    connect(w, &GnmiCallWorker::opened, this, &GnmiPanel::onOpened);
    connect(w, &GnmiCallWorker::openFailed, this, &GnmiPanel::onOpenFailed);
    connect(w, &GnmiCallWorker::getFinished, this, &GnmiPanel::onGetFinished);
    connect(w, &GnmiCallWorker::setFinished, this, &GnmiPanel::onSetFinished);
    connect(w, &GnmiCallWorker::closed, this, &GnmiPanel::onClosed);
    GnmiStreamWorker *s = m_conn->stream();
    connect(s, &GnmiStreamWorker::update, this, &GnmiPanel::onUpdate);
    connect(s, &GnmiStreamWorker::synced, this, &GnmiPanel::onSynced);
    connect(s, &GnmiStreamWorker::finished, this, &GnmiPanel::onSubscriptionFinished);
    m_clock.start();
    setSelectedNode(nullptr);
    updateUi();
}

GnmiPanel::~GnmiPanel() = default;

void GnmiPanel::buildUi()
{
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_connectBtn = new QPushButton(tr("Connect..."), this);
    m_disconnectBtn = new QPushButton(tr("Disconnect"), this);
    m_modelsBtn = new QPushButton(tr("Load Matching Schemas"), this);
    m_modelsBtn->setToolTip(tr("Load the modules the target lists in its capabilities from the search "
                               "directories (gNMI cannot download schemas)"));
    auto *bar = new QHBoxLayout;
    bar->addWidget(m_status, 1);
    bar->addWidget(m_modelsBtn);
    bar->addWidget(m_connectBtn);
    bar->addWidget(m_disconnectBtn);

    m_op = new QComboBox(this);
    m_op->addItem(QStringLiteral("Get"), int(Op::Get));
    m_op->addItem(QStringLiteral("Set"), int(Op::Set));
    m_op->addItem(QStringLiteral("Subscribe"), int(Op::Subscribe));
    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setForegroundRole(QPalette::PlaceholderText);
    m_selectionLabel->setTextFormat(Qt::PlainText);
    m_selectionLabel->setWordWrap(true);

    m_completions = new QStringListModel(this);
    m_completer = new QCompleter(m_completions, this);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setMaxVisibleItems(15);

    auto encodings = [this](QComboBox *c) {
        c->addItem(QStringLiteral("JSON_IETF"), int(Encoding::JsonIetf));
        c->addItem(QStringLiteral("JSON"), int(Encoding::JSON));
        c->addItem(QStringLiteral("PROTO"), int(Encoding::Proto));
        c->addItem(QStringLiteral("ASCII"), int(Encoding::Ascii));
    };

    // --- Get page
    auto *getPage = new QWidget(this);
    m_getPath = new QLineEdit(this);
    m_getPath->setFont(mono);
    m_getPath->setPlaceholderText(tr("/interfaces/interface[name=eth0]/state"));
    m_getPath->setCompleter(m_completer);
    m_getType = new QComboBox(this);
    m_getType->addItem(QStringLiteral("ALL"), int(DataType::All));
    m_getType->addItem(QStringLiteral("CONFIG"), int(DataType::Config));
    m_getType->addItem(QStringLiteral("STATE"), int(DataType::State));
    m_getType->addItem(QStringLiteral("OPERATIONAL"), int(DataType::Operational));
    m_getEncoding = new QComboBox(this);
    encodings(m_getEncoding);
    m_getBtn = new QPushButton(tr("Get"), this);
    m_getBtn->setDefault(true);
    auto *gf = new QFormLayout(getPage);
    gf->setContentsMargins(0, 0, 0, 0);
    gf->addRow(tr("Path"), m_getPath);
    auto *gopts = new QHBoxLayout;
    gopts->addWidget(m_getType);
    gopts->addWidget(m_getEncoding);
    gopts->addStretch();
    gopts->addWidget(m_getBtn);
    gf->addRow(tr("Type"), gopts);

    // --- Set page
    auto *setPage = new QWidget(this);
    m_setEditor = new QPlainTextEdit(this);
    m_setEditor->setFont(mono);
    m_setEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_setEditor->setPlaceholderText(tr("update  /system/hostname = r2\n"
                                       "replace /interfaces/interface[name=eth1] = {\"enabled\": true}\n"
                                       "delete  /system/server[name=ntp2][port=123]\n\n"
                                       "Values are JSON_IETF. Tip: right-click an entry in the Data tab "
                                       "and choose Edit This Entry (Set)."));
    m_jsonHl = nullptr;
    m_validateBtn = new QPushButton(tr("Validate Locally"), this);
    m_validateBtn->setToolTip(tr("Check the values against the loaded schemas and, when connected, the "
                                 "resulting configuration of the target"));
    m_setBtn = new QPushButton(tr("Validate && Set..."), this);
    auto *sl = new QVBoxLayout(setPage);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(m_setEditor, 1);
    auto *sb = new QHBoxLayout;
    sb->addStretch();
    sb->addWidget(m_validateBtn);
    sb->addWidget(m_setBtn);
    sl->addLayout(sb);

    // --- Subscribe page
    auto *subPage = new QWidget(this);
    m_subPaths = new QPlainTextEdit(this);
    m_subPaths->setFont(mono);
    m_subPaths->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_subPaths->setPlaceholderText(tr("one path per line, e.g.\n/interfaces/interface[name=eth0]/statistics"));
    m_listMode = new QComboBox(this);
    m_listMode->addItem(QStringLiteral("STREAM"), int(ListMode::Stream));
    m_listMode->addItem(QStringLiteral("ONCE"), int(ListMode::Once));
    m_listMode->addItem(QStringLiteral("POLL"), int(ListMode::Poll));
    m_subMode = new QComboBox(this);
    m_subMode->addItem(QStringLiteral("SAMPLE"), int(SubscriptionMode::Sample));
    m_subMode->addItem(QStringLiteral("ON_CHANGE"), int(SubscriptionMode::OnChange));
    m_subMode->addItem(QStringLiteral("TARGET_DEFINED"), int(SubscriptionMode::TargetDefined));
    m_interval = new QDoubleSpinBox(this);
    m_interval->setRange(0.1, 3600);
    m_interval->setDecimals(1);
    m_interval->setValue(10);
    m_interval->setSuffix(tr(" s"));
    m_suppress = new QCheckBox(tr("Suppress redundant"), this);
    m_updatesOnly = new QCheckBox(tr("Updates only"), this);
    m_updatesOnly->setToolTip(tr("Skip the initial snapshot"));
    m_subEncoding = new QComboBox(this);
    encodings(m_subEncoding);
    m_subscribeBtn = new QPushButton(tr("Subscribe"), this);
    m_pollBtn = new QPushButton(tr("Poll"), this);
    m_stopBtn = new QPushButton(tr("Stop"), this);
    auto *subl = new QVBoxLayout(subPage);
    subl->setContentsMargins(0, 0, 0, 0);
    subl->addWidget(m_subPaths, 1);
    auto *r1 = new QHBoxLayout;
    r1->addWidget(m_listMode);
    r1->addWidget(m_subMode);
    r1->addWidget(new QLabel(tr("every"), this));
    r1->addWidget(m_interval);
    r1->addWidget(m_subEncoding);
    r1->addStretch();
    subl->addLayout(r1);
    auto *r2 = new QHBoxLayout;
    r2->addWidget(m_suppress);
    r2->addWidget(m_updatesOnly);
    r2->addStretch();
    r2->addWidget(m_pollBtn);
    r2->addWidget(m_stopBtn);
    r2->addWidget(m_subscribeBtn);
    subl->addLayout(r2);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(getPage);
    m_pages->addWidget(setPage);
    m_pages->addWidget(subPage);

    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    auto *left = new QWidget(this);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    auto *opRow = new QHBoxLayout;
    opRow->addWidget(m_op);
    opRow->addStretch();
    opRow->addWidget(m_cancelBtn);
    ll->addLayout(opRow);
    ll->addWidget(m_selectionLabel);
    ll->addWidget(m_pages, 1);

    // --- results
    m_errorBanner = new QLabel(this);
    m_errorBanner->setWordWrap(true);
    m_errorBanner->setTextFormat(Qt::RichText);
    m_errorBanner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorBanner->hide();

    m_dataTree = new DataTreeWidget(this);
    m_dataTree->setGnmiMode(true);
    m_updates = new QTreeWidget(this);
    m_updates->setHeaderLabels({tr("Path"), tr("Type"), tr("Value")});
    m_updates->setRootIsDecorated(false);
    m_updates->setAlternatingRowColors(true);
    m_updates->setUniformRowHeights(true);
    m_updates->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_updates->setColumnWidth(0, 420);
    m_updates->setColumnWidth(1, 80);
    m_json = new QPlainTextEdit(this);
    m_json->setReadOnly(true);
    m_json->setFont(mono);
    m_json->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_jsonHl = new XmlHighlighter(m_json->document());
    m_validation = new QTextBrowser(this);
    m_validation->setPlaceholderText(tr("Choose Set and press Validate Locally."));

    m_livePage = new QWidget(this);
    m_live = new QTreeWidget(this);
    m_live->setHeaderLabels({tr("Path"), tr("Value"), tr("Rate /s"), tr("Updates"), tr("Last")});
    m_live->setRootIsDecorated(false);
    m_live->setAlternatingRowColors(true);
    m_live->setUniformRowHeights(true);
    m_live->setSortingEnabled(true);
    m_live->sortByColumn(0, Qt::AscendingOrder);
    m_live->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_live->setColumnWidth(0, 420);
    m_live->setColumnWidth(1, 160);
    m_graph = new LiveGraph(this);
    m_liveStatus = new QLabel(this);
    m_liveStatus->setForegroundRole(QPalette::PlaceholderText);
    auto *liveSplit = new QSplitter(Qt::Vertical, this);
    liveSplit->addWidget(m_live);
    liveSplit->addWidget(m_graph);
    liveSplit->setStretchFactor(0, 3);
    liveSplit->setStretchFactor(1, 2);
    auto *lvl = new QVBoxLayout(m_livePage);
    lvl->setContentsMargins(0, 0, 0, 0);
    lvl->addWidget(m_liveStatus);
    lvl->addWidget(liveSplit, 1);

    auto *capsPage = new QWidget(this);
    m_capsSummary = new QLabel(this);
    m_capsSummary->setWordWrap(true);
    m_models = new QTreeWidget(this);
    m_models->setHeaderLabels({tr("Model"), tr("Version"), tr("Organization")});
    m_models->setRootIsDecorated(false);
    m_models->setAlternatingRowColors(true);
    m_models->setSortingEnabled(true);
    m_models->setColumnWidth(0, 300);
    auto *cl = new QVBoxLayout(capsPage);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(m_capsSummary);
    cl->addWidget(m_models, 1);

    m_history = new QTreeWidget(this);
    m_history->setHeaderLabels({tr("Time"), tr("Request"), tr("ms"), tr("Result")});
    m_history->setRootIsDecorated(false);
    m_history->setAlternatingRowColors(true);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(m_dataTree, tr("Data"));
    m_tabs->addTab(m_updates, tr("Updates"));
    m_tabs->addTab(m_json, tr("JSON"));
    m_tabs->addTab(m_validation, tr("Validation"));
    m_tabs->addTab(m_livePage, tr("Live"));
    m_tabs->addTab(capsPage, tr("Capabilities"));
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

    // names for automation and tests
    m_getPath->setObjectName(QStringLiteral("gnmiGetPath"));
    m_getBtn->setObjectName(QStringLiteral("gnmiGetBtn"));
    m_setEditor->setObjectName(QStringLiteral("gnmiSetEditor"));
    m_validateBtn->setObjectName(QStringLiteral("gnmiValidateBtn"));
    m_setBtn->setObjectName(QStringLiteral("gnmiSetBtn"));
    m_subPaths->setObjectName(QStringLiteral("gnmiSubPaths"));
    m_subscribeBtn->setObjectName(QStringLiteral("gnmiSubscribeBtn"));
    m_stopBtn->setObjectName(QStringLiteral("gnmiStopBtn"));
    m_pollBtn->setObjectName(QStringLiteral("gnmiPollBtn"));
    m_listMode->setObjectName(QStringLiteral("gnmiListMode"));
    m_subMode->setObjectName(QStringLiteral("gnmiSubMode"));
    m_interval->setObjectName(QStringLiteral("gnmiInterval"));
    m_dataTree->setObjectName(QStringLiteral("gnmiDataTree"));
    m_live->setObjectName(QStringLiteral("gnmiLive"));
    m_graph->setObjectName(QStringLiteral("gnmiGraph"));
    m_validation->setObjectName(QStringLiteral("gnmiValidation"));
    m_tabs->setObjectName(QStringLiteral("gnmiTabs"));
    m_op->setObjectName(QStringLiteral("gnmiOp"));
    m_errorBanner->setObjectName(QStringLiteral("gnmiErrorBanner"));

    connect(m_connectBtn, &QPushButton::clicked, this, &GnmiPanel::connectDialog);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &GnmiPanel::disconnectTarget);
    connect(m_modelsBtn, &QPushButton::clicked, this, [this] {
        emit loadModelsRequested(m_caps.models, m_targetLabel);
    });
    connect(m_op, &QComboBox::currentIndexChanged, this, [this] {
        m_pages->setCurrentIndex(m_op->currentIndex());
        updateUi();
    });
    connect(m_getBtn, &QPushButton::clicked, this, &GnmiPanel::startGet);
    connect(m_getPath, &QLineEdit::returnPressed, this, &GnmiPanel::startGet);
    connect(m_getPath, &QLineEdit::textEdited, this, [this] { m_getEdited = true; });
    connect(m_validateBtn, &QPushButton::clicked, this, [this] { validateLocally(false); });
    connect(m_setBtn, &QPushButton::clicked, this, [this] { validateLocally(true); });
    connect(m_setEditor, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_settingText)
            m_setEdited = true;
    });
    connect(m_subPaths, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_settingText)
            m_subEdited = true;
    });
    connect(m_subscribeBtn, &QPushButton::clicked, this, &GnmiPanel::startSubscription);
    connect(m_stopBtn, &QPushButton::clicked, this, &GnmiPanel::stopSubscription);
    connect(m_pollBtn, &QPushButton::clicked, this, [this] {
        if (m_conn->poll())
            emit statusMessage(tr("Poll sent"));
    });
    connect(m_listMode, &QComboBox::currentIndexChanged, this, &GnmiPanel::updateUi);
    connect(m_subMode, &QComboBox::currentIndexChanged, this, &GnmiPanel::updateUi);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] { m_conn->cancelCall(); });
    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_setEditor);
    sc->setContext(Qt::WidgetShortcut);
    connect(sc, &QShortcut::activated, this, [this] { validateLocally(true); });
    connect(m_dataTree, &DataTreeWidget::schemaNodeActivated, this, &GnmiPanel::schemaNodeActivated);
    connect(m_dataTree, &DataTreeWidget::copyRequested, this, &GnmiPanel::copyRequested);
    connect(m_dataTree, &DataTreeWidget::entryAction, this, &GnmiPanel::onEntryAction);
    connect(m_live, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it)
            showGraphFor(it->data(0, Qt::UserRole).toString());
    });
    applyThemeColors(false);
}

void GnmiPanel::applyThemeColors(bool dark)
{
    m_jsonHl->updateColors(dark);
    m_errorBanner->setStyleSheet(dark
        ? QStringLiteral("QLabel { background: #450a0a; color: #fecaca; border: 1px solid #7f1d1d;"
                         " border-radius: 4px; padding: 6px; }")
        : QStringLiteral("QLabel { background: #fee2e2; color: #7f1d1d; border: 1px solid #fca5a5;"
                         " border-radius: 4px; padding: 6px; }"));
    m_graph->update();
}

void GnmiPanel::updateUi()
{
    if (m_connected)
        m_status->setText(tr("<b style='color:#16a34a'>&#9679;</b> %1").arg(m_targetLabel.toHtmlEscaped()));
    else if (m_busy)
        m_status->setText(tr("<b style='color:#d97706'>&#9679;</b> connecting..."));
    else
        m_status->setText(tr("<span style='color:gray'>&#9679; not connected</span>"));
    const bool idle = m_connected && !m_busy;
    m_connectBtn->setEnabled(!m_connected && !m_busy);
    m_disconnectBtn->setEnabled(m_connected || m_busy);
    m_modelsBtn->setEnabled(m_connected && !m_caps.models.isEmpty());
    m_getBtn->setEnabled(idle);
    m_setBtn->setEnabled(idle);
    m_validateBtn->setEnabled(!m_busy);
    m_cancelBtn->setVisible(m_busy);
    const ListMode lm = ListMode(m_listMode->currentData().toInt());
    const bool sample = SubscriptionMode(m_subMode->currentData().toInt()) == SubscriptionMode::Sample;
    m_subMode->setEnabled(lm == ListMode::Stream);
    m_interval->setEnabled(lm == ListMode::Stream && sample);
    m_suppress->setEnabled(lm == ListMode::Stream && sample);
    m_subscribeBtn->setEnabled(m_connected && !m_subscribed);
    m_stopBtn->setEnabled(m_subscribed);
    m_pollBtn->setVisible(lm == ListMode::Poll);
    m_pollBtn->setEnabled(m_subscribed && lm == ListMode::Poll);
}

void GnmiPanel::setEditorText(QPlainTextEdit *e, const QString &text)
{
    m_settingText = true;
    e->setPlainText(text);
    m_settingText = false;
}

void GnmiPanel::setLineText(QLineEdit *e, const QString &text)
{
    e->setText(text);
}

void GnmiPanel::showError(const QString &html)
{
    m_errorBanner->setText(html);
    m_errorBanner->show();
}

void GnmiPanel::addHistory(const QString &label, const QString &result, qint64 ms, bool ok)
{
    auto *it = new QTreeWidgetItem({QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), label,
                                    ms >= 0 ? QString::number(ms) : QString(), result});
    if (!ok)
        it->setForeground(3, QColor(0xdc, 0x26, 0x26));
    m_history->insertTopLevelItem(0, it);
    while (m_history->topLevelItemCount() > kMaxHistory)
        delete m_history->takeTopLevelItem(m_history->topLevelItemCount() - 1);
}

void GnmiPanel::setSelectedNode(const lysc_node *node)
{
    m_selected = node;
    if (!node) {
        m_selectionLabel->setText(tr("Select a node in the schema tree to get, set or subscribe to it."));
        return;
    }
    const QString path = pathToString(pathForSchemaNode(node, false));
    m_selectionLabel->setText(tr("From selection: %1").arg(path));
    if (!m_getEdited)
        setLineText(m_getPath, path);
    if (!m_subEdited)
        setEditorText(m_subPaths, path);
    if (!m_setEdited && !(node->flags & LYS_CONFIG_R))
        setEditorText(m_setEditor, skeletonFor(node));
}

void GnmiPanel::prepareFor(const lysc_node *node, Op op, bool run)
{
    m_getEdited = m_subEdited = m_setEdited = false;
    setSelectedNode(node);
    m_op->setCurrentIndex(m_op->findData(int(op)));
    if (!run)
        return;
    if (op == Op::Get)
        startGet();
    else if (op == Op::Subscribe)
        startSubscription();
}

// --- Get -------------------------------------------------------------------------------

void GnmiPanel::startGet()
{
    if (!m_connected || m_busy)
        return;
    const QString text = m_getPath->text().trimmed();
    Path p;
    QString err;
    if (!parsePath(text.isEmpty() ? QStringLiteral("/") : text, &p, &err)) {
        showError(tr("<b>Path:</b> %1").arg(err.toHtmlEscaped()));
        return;
    }
    m_errorBanner->hide();
    m_busy = true;
    m_pending = Pending::Get;
    m_pendingId = m_nextId++;
    m_pendingLabel = QStringLiteral("Get %1").arg(pathToString(p));
    updateUi();
    QMetaObject::invokeMethod(m_conn->calls(), "get", Qt::QueuedConnection, Q_ARG(int, m_pendingId),
                              Q_ARG(QStringList, QStringList{pathToString(p)}),
                              Q_ARG(int, m_getType->currentData().toInt()),
                              Q_ARG(int, m_getEncoding->currentData().toInt()));
    emit statusMessage(tr("Sent %1...").arg(m_pendingLabel));
}

void GnmiPanel::onGetFinished(int id, bool ok, const QVector<Notification> &notes, qint64 ms,
                              const QString &error)
{
    if (id != m_pendingId)
        return;
    const Pending what = m_pending;
    m_busy = false;
    m_pending = Pending::None;
    updateUi();

    if (what == Pending::ValidateFetch || what == Pending::SendFetch) {
        if (!ok && !error.startsWith(QLatin1String("NOT_FOUND"))) {
            showError(tr("<b>Could not read the current configuration:</b> %1").arg(error.toHtmlEscaped()));
            addHistory(m_pendingLabel, error, ms, false);
            return;
        }
        if (ok)
            m_fetched += notes;
        fetchNextConfig();
        return;
    }
    if (!ok) {
        showError(tr("<b>%1 failed:</b> %2").arg(m_pendingLabel.toHtmlEscaped(), error.toHtmlEscaped()));
        addHistory(m_pendingLabel, error, ms, false);
        emit logLines({QStringLiteral("gnmi: %1 failed: %2").arg(m_pendingLabel, error)});
        return;
    }
    int updates = 0;
    for (const Notification &n : notes)
        updates += int(n.updates.size());
    addHistory(m_pendingLabel, tr("%n update(s)", nullptr, updates), ms, true);
    showNotifications(notes);
    emit statusMessage(tr("%1: %n update(s) in %2 ms", nullptr, updates).arg(m_pendingLabel).arg(ms));
}

void GnmiPanel::showNotifications(const QVector<Notification> &notes)
{
    m_errorBanner->hide();
    m_lastNotes = notes;
    m_updates->clear();
    QList<QTreeWidgetItem *> rows;
    for (const Notification &n : notes) {
        for (const Update &u : n.updates) {
            if (rows.size() >= kMaxUpdateRows)
                break;
            auto *it = new QTreeWidgetItem({pathToString(u.path), u.value.kindName(), displayValue(u.value)});
            it->setToolTip(2, QString::fromUtf8(u.value.json).left(4000));
            rows << it;
        }
        for (const Path &d : n.deletes)
            rows << new QTreeWidgetItem({pathToString(d), tr("delete"), QString()});
    }
    m_updates->addTopLevelItems(rows);
    rebuildDataTree();
    m_tabs->setCurrentWidget(m_data.isEmpty() ? static_cast<QWidget *>(m_updates) : m_dataTree);
}

void GnmiPanel::rebuildDataTree()
{
    m_dataTree->clearTree();
    m_data.reset();
    m_json->clear();
    if (m_lastNotes.isEmpty())
        return;
    QStringList msgs;
    const int placed = notificationsToTree(m_ctx.raw(), m_lastNotes, &m_data, &msgs);
    QStringList problems;
    for (const QString &m : msgs)
        if (!m.startsWith(QLatin1String("deleted:")))
            problems << QStringLiteral("  ") + m;
    if (!problems.isEmpty()) {
        problems.prepend(tr("gnmi: %n update(s) could not be placed in the schema tree (see Updates tab):",
                            nullptr, int(problems.size())));
        emit logLines(problems.mid(0, 50));
    }
    Q_UNUSED(placed);
    m_dataTree->showTree(m_data);
    m_json->setPlainText(m_data.toJson());
}

// --- Set ---------------------------------------------------------------------------

bool GnmiPanel::parseScript(QVector<SetOperation> *ops)
{
    QStringList errors;
    if (!parseSetScript(m_setEditor->toPlainText(), ops, &errors)) {
        EditValidation v;
        v.errors = errors;
        m_validation->setHtml(validationHtml(v));
        m_tabs->setCurrentWidget(m_validation);
        return false;
    }
    return true;
}

void GnmiPanel::validateLocally(bool thenSend)
{
    if (m_busy)
        return;
    QVector<SetOperation> ops;
    if (!parseScript(&ops))
        return;
    m_errorBanner->hide();
    m_setOps = ops;
    m_sendAfterValidation = thenSend;
    if (!m_connected) {
        m_validation->setHtml(validationHtml(validateSet(m_ctx.raw(), ops, nullptr)));
        m_tabs->setCurrentWidget(m_validation);
        return;
    }
    // Read the current configuration of every top-level node the Set touches.
    QStringList tops;
    bool root = false;
    for (const SetOperation &op : ops) {
        if (op.path.isRoot()) {
            root = true;
            break;
        }
        Path t;
        t.origin = op.path.origin;
        t.elems = {PathElem{op.path.elems.first().name, {}}};
        const QString s = pathToString(t);
        if (!tops.contains(s))
            tops << s;
    }
    m_fetchQueue = root ? QStringList{QStringLiteral("/")} : tops;
    m_fetched.clear();
    fetchNextConfig();
}

void GnmiPanel::fetchNextConfig()
{
    if (m_fetchQueue.isEmpty()) {
        finishValidation();
        return;
    }
    const QString path = m_fetchQueue.takeFirst();
    m_busy = true;
    m_pending = m_sendAfterValidation ? Pending::SendFetch : Pending::ValidateFetch;
    m_pendingId = m_nextId++;
    m_pendingLabel = QStringLiteral("Get CONFIG %1 (for validation)").arg(path);
    updateUi();
    QMetaObject::invokeMethod(m_conn->calls(), "get", Qt::QueuedConnection, Q_ARG(int, m_pendingId),
                              Q_ARG(QStringList, QStringList{path}), Q_ARG(int, int(DataType::Config)),
                              Q_ARG(int, int(Encoding::JsonIetf)));
}

void GnmiPanel::finishValidation()
{
    DataTree current;
    QStringList msgs;
    notificationsToTree(m_ctx.raw(), m_fetched, &current, &msgs);
    const EditValidation v = validateSet(m_ctx.raw(), m_setOps, &current);
    m_validation->setHtml(validationHtml(v));
    m_tabs->setCurrentWidget(m_validation);
    if (!m_sendAfterValidation)
        return;
    bool go;
    if (!v.ok) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Set Not Valid"));
        box.setText(tr("Local validation found %n problem(s). The target will most likely reject "
                       "this Set.", nullptr, int(v.errors.size())));
        box.setDetailedText(v.errors.join(QLatin1Char('\n')));
        QPushButton *anyway = box.addButton(tr("Send Anyway"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        go = box.clickedButton() == static_cast<QAbstractButton *>(anyway);
    } else {
        go = confirmChangesDialog(this, tr("gNMI Set"),
                                  tr("Send this Set to <b>%1</b>?").arg(m_targetLabel.toHtmlEscaped()),
                                  v.changes, v.warnings, tr("Set"), v.changes.isEmpty());
    }
    if (!go)
        return;
    m_busy = true;
    m_pending = Pending::Set;
    m_pendingId = m_nextId++;
    m_pendingLabel = tr("Set (%n operation(s))", nullptr, int(m_setOps.size()));
    updateUi();
    QMetaObject::invokeMethod(m_conn->calls(), "set", Qt::QueuedConnection, Q_ARG(int, m_pendingId),
                              Q_ARG(QVector<ys::gnmi::SetOperation>, m_setOps));
}

void GnmiPanel::onSetFinished(int id, bool ok, const SetResult &res, qint64 ms, const QString &error)
{
    if (id != m_pendingId)
        return;
    m_busy = false;
    m_pending = Pending::None;
    updateUi();
    if (!ok) {
        showError(tr("<b>Set failed:</b> %1").arg(error.toHtmlEscaped()));
        addHistory(m_pendingLabel, error, ms, false);
        emit logLines({QStringLiteral("gnmi: Set failed: %1").arg(error)});
        return;
    }
    addHistory(m_pendingLabel, tr("ok, %n result(s)", nullptr, int(res.results.size())), ms, true);
    QStringList log{tr("gnmi: Set accepted by %1").arg(m_targetLabel)};
    for (const QString &r : res.results)
        log << QStringLiteral("  ") + r;
    emit logLines(log);
    m_validation->append(tr("<p style='color:#16a34a'><b>Set accepted</b> (%1 ms)</p><pre>%2</pre>")
                             .arg(ms).arg(res.results.join(QLatin1Char('\n')).toHtmlEscaped()));
    emit statusMessage(tr("Set accepted by %1").arg(m_targetLabel));
}

// --- Subscribe ------------------------------------------------------------------------

void GnmiPanel::startSubscription()
{
    if (!m_connected || m_subscribed)
        return;
    SubscribeSpec spec;
    spec.mode = ListMode(m_listMode->currentData().toInt());
    spec.encoding = Encoding(m_subEncoding->currentData().toInt());
    spec.updatesOnly = m_updatesOnly->isChecked();
    QStringList bad;
    for (const QString &line : m_subPaths->toPlainText().split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (t.isEmpty() || t.startsWith(QLatin1Char('#')))
            continue;
        SubscriptionSpec s;
        QString err;
        if (!parsePath(t, &s.path, &err)) {
            bad << QStringLiteral("%1: %2").arg(t, err);
            continue;
        }
        s.mode = SubscriptionMode(m_subMode->currentData().toInt());
        s.sampleIntervalNs = quint64(m_interval->value() * 1e9);
        s.suppressRedundant = m_suppress->isChecked();
        spec.subscriptions << s;
    }
    if (!bad.isEmpty() || spec.subscriptions.isEmpty()) {
        showError(bad.isEmpty() ? tr("Enter at least one path to subscribe to.")
                                : tr("<b>Path:</b> %1").arg(bad.join(QStringLiteral("<br>")).toHtmlEscaped()));
        return;
    }
    m_errorBanner->hide();
    m_series.clear();
    m_live->clear();
    m_graph->clearSeries();
    m_graphKey.clear();
    m_syncs = 0;
    m_subscribed = true;
    const QString modes = spec.mode == ListMode::Stream
        ? m_subMode->currentText() + (spec.subscriptions.first().mode == SubscriptionMode::Sample
                                          ? QStringLiteral(" %1 s").arg(m_interval->value()) : QString())
        : m_listMode->currentText();
    m_liveStatus->setText(tr("Subscribed (%1) to %n path(s); waiting for data...", nullptr,
                             int(spec.subscriptions.size())).arg(modes));
    addHistory(tr("Subscribe %1, %n path(s)", nullptr, int(spec.subscriptions.size())).arg(modes),
               tr("started"), -1, true);
    m_tabs->setCurrentWidget(m_livePage);
    updateUi();
    QMetaObject::invokeMethod(m_conn->stream(), "subscribe", Qt::QueuedConnection,
                              Q_ARG(ys::gnmi::SubscribeSpec, spec));
}

void GnmiPanel::stopSubscription()
{
    if (m_subscribed)
        m_conn->cancelSubscription();
}

void GnmiPanel::onUpdate(const Notification &n)
{
    const double now = m_clock.elapsed() / 1000.0;
    // the target's timestamps are used for rates when present
    const double ts = n.timestampNs > 0 ? double(n.timestampNs) / 1e9 : -1;
    const QString when = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_live->setSortingEnabled(false);
    for (const Update &u : n.updates) {
        const QString key = pathToString(u.path);
        Series &s = m_series[key];
        if (!s.item) {
            s.item = new QTreeWidgetItem(m_live);
            s.item->setText(0, key);
            s.item->setToolTip(0, key);
            s.item->setData(0, Qt::UserRole, key);
        }
        const QString text = displayValue(u.value);
        s.item->setText(1, text);
        s.item->setToolTip(1, text);
        s.item->setText(3, QString::number(++s.count));
        s.item->setText(4, when);
        double v = 0;
        const bool num = toNumber(text, &v);
        s.numeric = s.numeric && num;
        if (num) {
            const double t = ts > 0 ? ts : now;
            if (s.lastT >= 0 && t > s.lastT) {
                if (v < s.lastValue)
                    s.monotonic = false;
                s.rate = (v - s.lastValue) / (t - s.lastT);
                if (s.monotonic && s.count > 1)
                    s.item->setText(2, compactNumber(s.rate));
            }
            s.lastValue = v;
            s.lastT = t;
            s.values << QPointF(t, v);
            if (s.values.size() > kMaxPoints)
                s.values.remove(0, s.values.size() - kMaxPoints);
        }
    }
    for (const Path &d : n.deletes) {
        const QString key = pathToString(d);
        if (m_series.contains(key)) {
            m_series[key].item->setText(1, tr("(deleted)"));
        } else {
            auto *it = new QTreeWidgetItem(m_live, {key, tr("(deleted)")});
            it->setData(0, Qt::UserRole, key);
        }
    }
    m_live->setSortingEnabled(true);
    if (m_graphKey.isEmpty()) {
        // graph the first counter (in path order) that moves
        QStringList keys = m_series.keys();
        keys.sort();
        for (const QString &k : std::as_const(keys)) {
            const Series &s = m_series[k];
            if (s.numeric && s.monotonic && s.rate > 0) {
                m_graphKey = k;
                if (s.item) {
                    const QSignalBlocker b(m_live);
                    m_live->setCurrentItem(s.item);
                }
                break;
            }
        }
    }
    if (!m_graphKey.isEmpty())
        showGraphFor(m_graphKey);
    m_liveStatus->setText(tr("%n path(s), %1 sync(s)", nullptr, int(m_series.size())).arg(m_syncs));
}

void GnmiPanel::showGraphFor(const QString &key)
{
    auto it = m_series.constFind(key);
    if (it == m_series.constEnd() || !it->numeric) {
        m_graph->setSeries(lastName(key), QString(), {});
        return;
    }
    m_graphKey = key;
    const Series &s = *it;
    const bool counter = s.monotonic && s.values.size() >= 3 && s.values.last().y() > s.values.first().y();
    if (!counter) {
        m_graph->setSeries(lastName(key), QString(), s.values);
        return;
    }
    QVector<QPointF> rates;
    for (int i = 1; i < s.values.size(); ++i) {
        const double dt = s.values[i].x() - s.values[i - 1].x();
        if (dt > 0)
            rates << QPointF(s.values[i].x(), (s.values[i].y() - s.values[i - 1].y()) / dt);
    }
    m_graph->setSeries(lastName(key) + tr(" rate"), tr("/s"), rates);
}

void GnmiPanel::onSynced()
{
    ++m_syncs;
    m_liveStatus->setText(tr("%n path(s), initial data complete", nullptr, int(m_series.size())));
    emit subscriptionSynced();
}

void GnmiPanel::onSubscriptionFinished(bool ok, const QString &error)
{
    m_subscribed = false;
    updateUi();
    if (!ok) {
        showError(tr("<b>Subscription ended:</b> %1").arg(error.toHtmlEscaped()));
        addHistory(tr("Subscribe"), error, -1, false);
        emit logLines({QStringLiteral("gnmi: subscription ended: %1").arg(error)});
    } else {
        m_liveStatus->setText(tr("Subscription ended; %n path(s) received", nullptr, int(m_series.size())));
        addHistory(tr("Subscribe"), tr("ended"), -1, true);
    }
}

// --- data tree actions ---------------------------------------------------------------

void GnmiPanel::onEntryAction(const lyd_node *node, DataTreeWidget::EntryAction action)
{
    using A = DataTreeWidget::EntryAction;
    const Path path = pathForDataNode(node, false);
    const QString ps = pathToString(path);
    switch (action) {
    case A::FilterSubtree:
        m_op->setCurrentIndex(m_op->findData(int(Op::Get)));
        m_getEdited = true;
        setLineText(m_getPath, ps);
        startGet();
        break;
    case A::FilterXPath:
        m_op->setCurrentIndex(m_op->findData(int(Op::Subscribe)));
        m_subEdited = true;
        setEditorText(m_subPaths, ps);
        emit statusMessage(tr("Choose the mode and press Subscribe"));
        break;
    case A::Edit:
    case A::Delete: {
        SetOperation op;
        op.path = path;
        op.kind = action == A::Edit ? SetOperation::Replace : SetOperation::Delete;
        if (action == A::Edit)
            op.json = entryJson(node);
        m_op->setCurrentIndex(m_op->findData(int(Op::Set)));
        setEditorText(m_setEditor, formatSetOperation(op));
        m_setEdited = true;
        m_setEditor->setFocus();
        emit statusMessage(action == A::Edit ? tr("Change the values, then Validate & Set")
                                             : tr("Review the delete, then Validate & Set"));
        break;
    }
    }
}

// --- schema context ---------------------------------------------------------------------

void GnmiPanel::contextAboutToReset()
{
    m_dataTree->clearTree();
    m_data.reset();
    m_selected = nullptr;
    m_completions->setStringList({});
}

void GnmiPanel::contextChanged()
{
    m_selected = nullptr;
    setSelectedNode(nullptr);
    rebuildDataTree();
    rebuildCompletions();
}

void GnmiPanel::rebuildCompletions()
{
    QStringList paths;
    std::function<void(const lysc_node *)> walk = [&](const lysc_node *n) {
        if (paths.size() >= kMaxCompletions)
            return;
        if (!(n->nodetype & (LYS_CHOICE | LYS_CASE | LYS_RPC | LYS_ACTION | LYS_NOTIF)))
            paths << pathToString(pathForSchemaNode(n, false));
        if (n->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF))
            return;
        for (const lysc_node *c : childrenOf(n))
            walk(c);
    };
    for (const lys_module *m : m_ctx.implementedModules())
        for (const lysc_node *t : topLevelOf(m))
            walk(t);
    paths.removeDuplicates();
    paths.sort();
    m_completions->setStringList(paths);
}

// --- connection ---------------------------------------------------------------------------

void GnmiPanel::connectDialog()
{
    GnmiConnectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    connectTo(dlg.params(), dlg.profileName());
}

void GnmiPanel::connectTo(const ConnectParams &params, const QString &profile)
{
    if (m_connected || m_busy)
        return;
    if (!profile.isEmpty())
        QSettings().setValue(QStringLiteral("gnmiTargets.last"), profile);
    m_targetLabel = QStringLiteral("%1%2:%3%4")
                        .arg(params.username.isEmpty() ? QString() : params.username + QLatin1Char('@'),
                             params.host).arg(params.port)
                        .arg(params.tls == TlsMode::Insecure ? tr(" (insecure)") : QString());
    m_busy = true;
    m_errorBanner->hide();
    updateUi();
    emit logLines({tr("gnmi: connecting to %1").arg(m_targetLabel)});
    QMetaObject::invokeMethod(m_conn->calls(), "open", Qt::QueuedConnection,
                              Q_ARG(ys::gnmi::ConnectParams, params));
}

void GnmiPanel::onOpened(const Capabilities &caps)
{
    m_busy = false;
    m_connected = true;
    m_caps = caps;
    m_models->clear();
    for (const ModelData &m : caps.models)
        m_models->addTopLevelItem(new QTreeWidgetItem({m.name, m.version, m.organization}));
    QStringList enc;
    for (Encoding e : caps.encodings)
        enc << encodingName(e);
    m_capsSummary->setText(tr("gNMI %1 &middot; %n model(s) &middot; encodings: %2", nullptr,
                              int(caps.models.size()))
                               .arg(caps.version.toHtmlEscaped(), enc.join(QStringLiteral(", "))));
    // pick an encoding the target supports
    for (QComboBox *c : {m_getEncoding, m_subEncoding}) {
        const Encoding cur = Encoding(c->currentData().toInt());
        if (!caps.encodings.isEmpty() && !caps.encodings.contains(cur)) {
            for (Encoding pref : {Encoding::JsonIetf, Encoding::JSON, Encoding::Proto})
                if (caps.encodings.contains(pref)) {
                    c->setCurrentIndex(c->findData(int(pref)));
                    break;
                }
        }
    }
    emit logLines({tr("gnmi: connected to %1, gNMI %2, %n model(s)", nullptr, int(caps.models.size()))
                       .arg(m_targetLabel, caps.version)});
    updateUi();
    emit connectionChanged(true);
}

void GnmiPanel::onOpenFailed(const QString &error)
{
    m_busy = false;
    m_connected = false;
    showError(tr("<b>Could not connect to %1:</b> %2").arg(m_targetLabel.toHtmlEscaped(), error.toHtmlEscaped()));
    emit logLines({tr("gnmi: connect failed: %1").arg(error)});
    updateUi();
}

void GnmiPanel::disconnectTarget()
{
    m_conn->cancelSubscription();
    m_conn->cancelCall();
    QMetaObject::invokeMethod(m_conn->calls(), "close", Qt::QueuedConnection);
}

void GnmiPanel::onClosed()
{
    const bool was = m_connected;
    m_connected = false;
    m_busy = false;
    m_caps = Capabilities();
    m_models->clear();
    m_capsSummary->clear();
    emit logLines({tr("gnmi: disconnected")});
    updateUi();
    if (was)
        emit connectionChanged(false);
}

} // namespace ys
