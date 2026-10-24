// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include "core/NodeInfo.h"
#include "gui/SchemaModel.h"
#include "gui/SearchPanel.h"
#include "gui/YangHighlighter.h"

#include <libyang/libyang.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>

namespace ys {

namespace {
constexpr int kMaxRecent = 8;

bool isDarkPalette()
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("YangScope"));
    setAcceptDrops(true);
    buildUi();
    buildMenus();

    QSettings s;
    if (!restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1280, 820);
    restoreState(s.value(QStringLiteral("window/state")).toByteArray());
    m_splitter->restoreState(s.value(QStringLiteral("window/splitter")).toByteArray());
    m_allFeaturesAct->setChecked(s.value(QStringLiteral("load/allFeatures"), true).toBool());
    m_recursiveAct->setChecked(s.value(QStringLiteral("load/recursive"), true).toBool());
    m_internalAct->setChecked(s.value(QStringLiteral("view/internalModules"), false).toBool());
    m_ctx.setEnableAllFeatures(m_allFeaturesAct->isChecked());

    if (!m_ctx.isValid())
        log(QStringLiteral("error: ") + m_ctx.lastError());
    log(tr("YangScope %1, libyang %2").arg(QStringLiteral(YANGSCOPE_VERSION),
                                           YangContext::libyangVersion()));
    refreshAfterLoad(QString());
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    m_model = new SchemaModel(this);
    m_tree = new QTreeView(this);
    m_tree->setModel(m_model);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(SchemaModel::ColName, QHeaderView::Interactive);
    m_tree->setColumnWidth(SchemaModel::ColName, 320);
    m_tree->setColumnWidth(SchemaModel::ColKind, 90);
    m_tree->setColumnWidth(SchemaModel::ColAccess, 55);
    m_tree->setColumnWidth(SchemaModel::ColType, 160);
    m_tree->setColumnWidth(SchemaModel::ColModule, 160);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MainWindow::onCurrentChanged);
    connect(m_tree, &QTreeView::customContextMenuRequested,
            this, &MainWindow::showTreeContextMenu);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_details = new QTextBrowser(this);
    m_details->setOpenLinks(false);

    m_source = new QPlainTextEdit(this);
    m_source->setReadOnly(true);
    m_source->setFont(mono);
    m_source->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_sourceHl = new YangHighlighter(m_source->document());

    m_treeText = new QPlainTextEdit(this);
    m_treeText->setReadOnly(true);
    m_treeText->setFont(mono);
    m_treeText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_treeHl = new YangHighlighter(m_treeText->document());

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_details, tr("Details"));
    m_tabs->addTab(m_source, tr("Source"));
    m_tabs->addTab(m_treeText, tr("Tree"));
    m_tabs->setDocumentMode(true);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_tree);
    m_splitter->addWidget(m_tabs);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    setCentralWidget(m_splitter);

    m_search = new SearchPanel(m_ctx, this);
    m_searchDock = new QDockWidget(tr("Search"), this);
    m_searchDock->setObjectName(QStringLiteral("searchDock"));
    m_searchDock->setWidget(m_search);
    addDockWidget(Qt::BottomDockWidgetArea, m_searchDock);
    connect(m_search, &SearchPanel::nodeActivated, this, [this](const lysc_node *n) {
        const QModelIndex idx = m_model->indexForNode(n);
        if (idx.isValid()) {
            m_tree->setCurrentIndex(idx);
            m_tree->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        } else {
            showNode(n);
        }
    });

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(mono);
    m_log->setMaximumBlockCount(5000);
    m_logDock = new QDockWidget(tr("Messages"), this);
    m_logDock->setObjectName(QStringLiteral("logDock"));
    m_logDock->setWidget(m_log);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    tabifyDockWidget(m_searchDock, m_logDock);
    m_searchDock->raise();

    m_statusModules = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusModules);

    applyThemeColors();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *openFiles = file->addAction(tr("&Open YANG Files..."), this, &MainWindow::openFilesDialog);
    openFiles->setShortcut(QKeySequence::Open);
    QAction *openFolder = file->addAction(tr("Open &Folder..."), this, &MainWindow::openFolderDialog);
    openFolder->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    file->addAction(tr("Add &Search Directory..."), this, &MainWindow::addSearchDirDialog);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    QAction *reload = file->addAction(tr("&Reload All"), this, &MainWindow::reloadAll);
    reload->setShortcut(QKeySequence::Refresh);
    file->addAction(tr("&Close All"), this, &MainWindow::closeAll);
    file->addSeparator();
    file->addAction(tr("&Export Tree Text..."), this, &MainWindow::exportTreeText);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    QAction *find = view->addAction(tr("&Find Node..."), this, [this] {
        m_searchDock->show();
        m_searchDock->raise();
        m_search->focusSearch();
    });
    find->setShortcut(QKeySequence::Find);
    view->addAction(tr("&Collapse All"), m_tree, &QTreeView::collapseAll);
    view->addSeparator();
    view->addAction(m_searchDock->toggleViewAction());
    view->addAction(m_logDock->toggleViewAction());

    QMenu *opts = menuBar()->addMenu(tr("&Options"));
    m_allFeaturesAct = opts->addAction(tr("Enable All &Features"));
    m_allFeaturesAct->setCheckable(true);
    m_allFeaturesAct->setToolTip(tr("Load modules with every feature enabled (takes effect on reload)"));
    connect(m_allFeaturesAct, &QAction::toggled, this, [this](bool on) {
        m_ctx.setEnableAllFeatures(on);
        QSettings().setValue(QStringLiteral("load/allFeatures"), on);
        if (!m_loadedFiles.isEmpty())
            reloadAll();
    });
    m_recursiveAct = opts->addAction(tr("Folders Load &Recursively"));
    m_recursiveAct->setCheckable(true);
    connect(m_recursiveAct, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("load/recursive"), on);
    });
    m_internalAct = opts->addAction(tr("Show libyang &Internal Modules"));
    m_internalAct->setCheckable(true);
    connect(m_internalAct, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("view/internalModules"), on);
        refreshAfterLoad(QString());
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About YangScope"), this, &MainWindow::about);
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);

    QToolBar *tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("mainToolbar"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->addAction(openFiles);
    tb->addAction(openFolder);
    tb->addAction(reload);
    tb->addSeparator();
    tb->addAction(find);

    updateRecentMenu();
}

// --- loading ---------------------------------------------------------------

void MainWindow::openFilesDialog()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open YANG Files"), s.value(QStringLiteral("load/lastDir")).toString(),
        tr("YANG modules (*.yang *.yin);;All files (*)"));
    if (files.isEmpty())
        return;
    s.setValue(QStringLiteral("load/lastDir"), QFileInfo(files.first()).absolutePath());
    openPaths(files, false);
}

void MainWindow::openFolderDialog()
{
    QSettings s;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Folder with YANG Modules"), s.value(QStringLiteral("load/lastDir")).toString());
    if (dir.isEmpty())
        return;
    s.setValue(QStringLiteral("load/lastDir"), dir);
    openPaths({dir}, m_recursiveAct->isChecked());
}

void MainWindow::addSearchDirDialog()
{
    QSettings s;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Add Search Directory for Imports"), s.value(QStringLiteral("load/lastDir")).toString());
    if (dir.isEmpty())
        return;
    addSearchDirs({dir}, m_recursiveAct->isChecked());
}

void MainWindow::addSearchDirs(const QStringList &dirs, bool recursive)
{
    int added = 0;
    for (const QString &d : dirs)
        added += m_ctx.addSearchDir(d, recursive);
    log(tr("added %n search director(y/ies)", nullptr, added));
}

void MainWindow::openPaths(const QStringList &paths, bool recursive)
{
    QStringList files;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir()) {
            m_ctx.addSearchDir(fi.absoluteFilePath(), recursive);
            files << YangContext::yangFilesIn(fi.absoluteFilePath(), recursive);
            rememberRecent(fi.absoluteFilePath());
        } else if (fi.isFile()) {
            // Register the folder up front so files opened together can
            // import each other regardless of their order.
            m_ctx.addSearchDir(fi.absolutePath(), false);
            files << fi.absoluteFilePath();
            rememberRecent(fi.absoluteFilePath());
        } else {
            log(tr("warning: %1 does not exist").arg(p));
        }
    }
    loadFiles(files);
}

void MainWindow::loadFiles(const QStringList &files)
{
    if (files.isEmpty())
        return;
    if (!m_ctx.isValid()) {
        QMessageBox::critical(this, tr("YangScope"),
                              tr("libyang could not be initialised:\n%1").arg(m_ctx.lastError()));
        return;
    }

    QProgressDialog progress(tr("Loading YANG modules..."), tr("Cancel"), 0, int(files.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);

    int ok = 0, failed = 0, skipped = 0;
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < files.size(); ++i) {
        progress.setValue(i);
        progress.setLabelText(QFileInfo(files[i]).fileName());
        if (progress.wasCanceled())
            break;
        const LoadResult r = m_ctx.loadFile(files[i]);
        if (r.skipped) {
            ++skipped;
            continue;
        }
        if (r.ok) {
            ++ok;
            if (!m_loadedFiles.contains(files[i]))
                m_loadedFiles << files[i];
        } else {
            ++failed;
            log(tr("failed: %1").arg(QDir::toNativeSeparators(files[i])));
        }
        QStringList msgs;
        for (const QString &m : r.messages)
            msgs << QStringLiteral("  ") + m;
        if (!msgs.isEmpty())
            log(msgs);
    }
    progress.setValue(int(files.size()));
    const qint64 ms = QDateTime::currentMSecsSinceEpoch() - t0;

    QString summary = tr("Loaded %n file(s)", nullptr, ok);
    if (skipped)
        summary += tr(", %n submodule(s) via their module", nullptr, skipped);
    if (failed)
        summary += tr(", %n failed (see Messages)", nullptr, failed);
    summary += tr(" in %1 ms").arg(ms);
    log(summary);
    if (failed) {
        m_logDock->show();
        m_logDock->raise();
    }
    refreshAfterLoad(summary);
}

void MainWindow::reloadAll()
{
    const QStringList files = m_loadedFiles;
    m_loadedFiles.clear();
    m_model->clear();
    m_ctx.reset();
    log(tr("reloading..."));
    loadFiles(files);
    if (files.isEmpty())
        refreshAfterLoad(QString());
}

void MainWindow::closeAll()
{
    m_loadedFiles.clear();
    m_model->clear();
    m_ctx.reset();
    refreshAfterLoad(tr("All modules closed"));
}

void MainWindow::refreshAfterLoad(const QString &summary)
{
    m_model->rebuild(m_ctx, m_internalAct->isChecked());
    m_search->invalidate();
    m_shownModule = nullptr;
    m_details->setHtml(m_model->moduleCount() == 0
        ? tr("<h2>No modules loaded</h2>"
             "<p>Use <b>File &rarr; Open Folder</b> or <b>Open YANG Files</b>, "
             "or drop .yang files or folders on this window.</p>"
             "<p>Imports are resolved from the folder of each file and from the "
             "search directories (<b>File &rarr; Add Search Directory</b>).</p>")
        : QString());
    m_source->clear();
    m_treeText->clear();
    m_statusModules->setText(tr("%n implemented module(s)", nullptr, m_model->moduleCount())
                             + QStringLiteral("  |  ")
                             + tr("%n in context", nullptr, int(m_ctx.allModules().size())));
    if (!summary.isEmpty())
        statusBar()->showMessage(summary, 8000);
    if (m_model->moduleCount() > 0 && !m_tree->currentIndex().isValid())
        m_tree->setCurrentIndex(m_model->index(0, 0));
}

// --- selection and display -------------------------------------------------

void MainWindow::onCurrentChanged()
{
    const QModelIndex idx = m_tree->currentIndex();
    if (!idx.isValid())
        return;
    if (const lysc_node *n = m_model->nodeAt(idx))
        showNode(n);
    else
        showModule(m_model->moduleAt(idx));
}

void MainWindow::showNode(const lysc_node *node)
{
    m_details->setHtml(nodeDetailsHtml(node));
    // Source and tree tabs follow the module the node is defined in.
    updateModuleTabs(node->module);
}

void MainWindow::showModule(const lys_module *mod)
{
    if (!mod)
        return;
    m_details->setHtml(moduleDetailsHtml(mod));
    updateModuleTabs(mod);
}

void MainWindow::updateModuleTabs(const lys_module *mod)
{
    if (!mod || mod == m_shownModule)
        return;
    m_shownModule = mod;
    m_source->setPlainText(moduleSource(mod));
    m_treeText->setPlainText(moduleTreeText(mod));
}

bool MainWindow::selectPath(const QString &path)
{
    if (!m_ctx.isValid())
        return false;
    QString p = path;
    p.remove(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]"))); // drop key predicates
    const QByteArray b = p.toUtf8();
    const lysc_node *n = lys_find_path(m_ctx.raw(), nullptr, b.constData(), 0);
    if (!n)
        n = lys_find_path(m_ctx.raw(), nullptr, b.constData(), 1);
    if (!n) {
        log(tr("path not found: %1").arg(path));
        return false;
    }
    const QModelIndex idx = m_model->indexForNode(n);
    if (!idx.isValid())
        return false;
    m_tree->setCurrentIndex(idx);
    m_tree->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    return true;
}

void MainWindow::find(const QString &text)
{
    m_searchDock->show();
    m_searchDock->raise();
    m_search->setQuery(text);
}

void MainWindow::showTreeContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_tree->indexAt(pos);
    if (!idx.isValid())
        return;
    QMenu menu(this);
    const lysc_node *n = m_model->nodeAt(idx);
    if (n) {
        struct Entry { QString label; QString value; };
        const QVector<Entry> entries = {
            {tr("Copy XPath"), dataPath(n)},
            {tr("Copy XPath with Keys"), dataPathWithKeys(n)},
            {tr("Copy Schema Node Id"), schemaPath(n)},
            {tr("Copy gNMI Path"), gnmiPath(n, false)},
            {tr("Copy gNMI Path (rfc7951)"), gnmiPath(n, true)},
            {tr("Copy RESTCONF Path"), restconfPath(n)},
            {tr("Copy NETCONF Subtree Filter"), subtreeFilter(n)},
        };
        for (const Entry &e : entries) {
            QAction *a = menu.addAction(e.label);
            a->setEnabled(!e.value.isEmpty());
            const QString value = e.value;
            const QString what = e.label.mid(5);
            connect(a, &QAction::triggered, this, [this, value, what] { copyToClipboard(value, what); });
        }
        menu.addSeparator();
    }
    menu.addAction(tr("Expand Subtree"), this, [this, idx] { m_tree->expandRecursively(idx, 8); });
    menu.addAction(tr("Collapse Subtree"), this, [this, idx] {
        m_tree->collapse(idx);
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MainWindow::copyToClipboard(const QString &text, const QString &what)
{
    QApplication::clipboard()->setText(text);
    statusBar()->showMessage(tr("Copied %1").arg(what), 3000);
}

void MainWindow::exportTreeText()
{
    const QModelIndex idx = m_tree->currentIndex();
    const lys_module *mod = idx.isValid() ? m_model->moduleAt(idx) : nullptr;
    if (!mod) {
        QMessageBox::information(this, tr("Export Tree"), tr("Select a module first."));
        return;
    }
    const QString suggested = QString::fromUtf8(mod->name) + QStringLiteral(".tree.txt");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Tree Text"), suggested,
                                                      tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export Tree"), f.errorString());
        return;
    }
    f.write(moduleTreeText(mod).toUtf8());
    if (!f.commit())
        QMessageBox::warning(this, tr("Export Tree"), f.errorString());
    else
        statusBar()->showMessage(tr("Wrote %1").arg(QDir::toNativeSeparators(path)), 5000);
}

void MainWindow::log(const QStringList &lines)
{
    for (const QString &l : lines)
        m_log->appendPlainText(l);
}

// --- recent files ------------------------------------------------------------

void MainWindow::rememberRecent(const QString &path)
{
    QSettings s;
    QStringList recent = s.value(QStringLiteral("recent")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecent)
        recent.removeLast();
    s.setValue(QStringLiteral("recent"), recent);
    updateRecentMenu();
}

void MainWindow::updateRecentMenu()
{
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(QStringLiteral("recent")).toStringList();
    for (const QString &p : recent) {
        m_recentMenu->addAction(QDir::toNativeSeparators(p), this, [this, p] {
            openPaths({p}, m_recursiveAct->isChecked());
        });
    }
    if (!recent.isEmpty()) {
        m_recentMenu->addSeparator();
        m_recentMenu->addAction(tr("Clear List"), this, [this] {
            QSettings().remove(QStringLiteral("recent"));
            updateRecentMenu();
        });
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

// --- misc ----------------------------------------------------------------------

void MainWindow::applyThemeColors()
{
    const bool dark = isDarkPalette();
    m_sourceHl->updateColors(dark);
    m_treeHl->updateColors(dark);
}

void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange)
        applyThemeColors();
    QMainWindow::changeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    if (!paths.isEmpty()) {
        e->acceptProposedAction();
        openPaths(paths, m_recursiveAct->isChecked());
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/state"), saveState());
    s.setValue(QStringLiteral("window/splitter"), m_splitter->saveState());
    QMainWindow::closeEvent(e);
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About YangScope"),
        tr("<h3>YangScope %1</h3>"
           "<p>A native YANG schema explorer, with NETCONF and gNMI support planned.</p>"
           "<p>Built with Qt %2 and libyang %3 (CESNET).</p>"
           "<p>Licensed under the GNU GPL v3 or later.</p>")
            .arg(QStringLiteral(YANGSCOPE_VERSION), QString::fromLatin1(qVersion()),
                 YangContext::libyangVersion()));
}

} // namespace ys
