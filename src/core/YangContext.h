// SPDX-License-Identifier: GPL-3.0-or-later
// YangScope - native YANG / NETCONF / gNMI explorer
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct ly_ctx;
struct lys_module;

namespace ys {

/// Result of a load operation: the module (if any) plus all messages
/// libyang produced while loading it.
struct LoadResult {
    const lys_module *module = nullptr;
    bool ok = false;
    bool skipped = false; ///< file is a submodule, loaded via its main module
    QStringList messages;
};

/// Owns a libyang context and the set of search directories.
/// All schema pointers handed out stay valid until reset() or destruction.
class YangContext {
public:
    YangContext();
    ~YangContext();
    YangContext(const YangContext &) = delete;
    YangContext &operator=(const YangContext &) = delete;

    /// Drop all loaded modules and start with a fresh context
    /// (search directories are kept).
    bool reset();

    /// Add a directory to look up imports in. With @p recursive every
    /// sub-directory that contains .yang files is added as well.
    /// Returns the number of directories added.
    int addSearchDir(const QString &dir, bool recursive);
    QStringList searchDirs() const { return m_searchDirs; }

    /// Parse a .yang file and make the module implemented.
    LoadResult loadFile(const QString &path);
    /// Load a module by name via the search directories.
    LoadResult loadModule(const QString &name, const QString &revision = {});

    /// List .yang files in a directory (optionally recursive).
    static QStringList yangFilesIn(const QString &dir, bool recursive);
    /// True if the file's first statement is "submodule".
    static bool isSubmoduleFile(const QString &path);

    /// Implemented modules (those that contribute data nodes), sorted by name.
    /// libyang's built-in modules are left out unless @p includeInternal.
    QVector<const lys_module *> implementedModules(bool includeInternal = false) const;
    /// All modules in the context, including import-only ones.
    QVector<const lys_module *> allModules(bool includeInternal = false) const;

    /// Enable all features ("*") of modules loaded from now on.
    void setEnableAllFeatures(bool on) { m_allFeatures = on; }
    bool enableAllFeatures() const { return m_allFeatures; }

    ly_ctx *raw() const { return m_ctx; }
    bool isValid() const { return m_ctx != nullptr; }
    /// Why the context could not be created, if it could not.
    QString lastError() const { return m_lastError; }

    static QString libyangVersion();

private:
    bool createContext();
    void applySearchDirs();

    ly_ctx *m_ctx = nullptr;
    QStringList m_searchDirs;
    QString m_lastError;
    bool m_allFeatures = true;
};

} // namespace ys
