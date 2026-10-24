// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/YangContext.h"

#include <QMainWindow>
#include <QStringList>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QSplitter;
class QTabWidget;
class QTextBrowser;
class QTreeView;
struct lys_module;
struct lysc_node;

namespace ys {

class SchemaModel;
class SearchPanel;
class YangHighlighter;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Load files and/or directories (directories: all .yang files inside,
    /// and the directory tree is added to the search path).
    void openPaths(const QStringList &paths, bool recursive);
    void addSearchDirs(const QStringList &dirs, bool recursive);
    /// Select a node by data path or schema path, e.g. "/ietf-interfaces:interfaces".
    bool selectPath(const QString &path);
    /// Show the search panel with @p text searched.
    void find(const QString &text);

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void buildUi();
    void buildMenus();
    void openFilesDialog();
    void openFolderDialog();
    void addSearchDirDialog();
    void reloadAll();
    void closeAll();
    void loadFiles(const QStringList &files);
    void refreshAfterLoad(const QString &summary);
    void onCurrentChanged();
    void showNode(const lysc_node *node);
    void showModule(const lys_module *mod);
    void updateModuleTabs(const lys_module *mod);
    void showTreeContextMenu(const QPoint &pos);
    void copyToClipboard(const QString &text, const QString &what);
    void exportTreeText();
    void log(const QStringList &lines);
    void log(const QString &line) { log(QStringList{line}); }
    void updateRecentMenu();
    void rememberRecent(const QString &path);
    void applyThemeColors();
    void about();

    YangContext m_ctx;
    QStringList m_loadedFiles;           // in load order, for reload
    const lys_module *m_shownModule = nullptr;

    SchemaModel *m_model = nullptr;
    QTreeView *m_tree = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTextBrowser *m_details = nullptr;
    QPlainTextEdit *m_source = nullptr;
    QPlainTextEdit *m_treeText = nullptr;
    YangHighlighter *m_sourceHl = nullptr;
    YangHighlighter *m_treeHl = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QDockWidget *m_searchDock = nullptr;
    QDockWidget *m_logDock = nullptr;
    SearchPanel *m_search = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_statusModules = nullptr;
    QMenu *m_recentMenu = nullptr;
    QAction *m_allFeaturesAct = nullptr;
    QAction *m_recursiveAct = nullptr;
    QAction *m_internalAct = nullptr;
};

} // namespace ys
