// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/YangContext.h"

#include <libyang/libyang.h>
#include <libyang/version.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace ys {

namespace {

// libyang reports problems through a process-wide callback. We collect them
// per thread and hand them back with the result of each load operation.
thread_local QStringList *t_sink = nullptr;

void appendMessage(LY_LOG_LEVEL level, const char *msg, const char *path)
{
    if (!t_sink || !msg || level > LY_LLWRN) // keep errors and warnings only
        return;
    QString line;
    switch (level) {
    case LY_LLERR: line = QStringLiteral("error: "); break;
    case LY_LLWRN: line = QStringLiteral("warning: "); break;
    default:       line = QStringLiteral("info: "); break;
    }
    line += QString::fromUtf8(msg);
    if (path && *path)
        line += QStringLiteral(" (") + QString::fromUtf8(path) + QLatin1Char(')');
    t_sink->append(line);
}

#ifdef YANGSCOPE_LY_LOG_CLB_V3
void logCallback(LY_LOG_LEVEL level, const char *msg, const char *dataPath,
                 const char *schemaPath, uint64_t line)
{
    QByteArray where;
    if (schemaPath && *schemaPath)
        where = schemaPath;
    else if (dataPath && *dataPath)
        where = dataPath;
    if (line)
        where += (where.isEmpty() ? "line " : ", line ") + QByteArray::number(qulonglong(line));
    appendMessage(level, msg, where.isEmpty() ? nullptr : where.constData());
}
#else
void logCallback(LY_LOG_LEVEL level, const char *msg, const char *path)
{
    appendMessage(level, msg, path);
}
#endif

struct LogCapture {
    explicit LogCapture(QStringList *sink) : prev(t_sink) { t_sink = sink; }
    ~LogCapture() { t_sink = prev; }
    QStringList *prev;
};

void installLoggerOnce()
{
    static bool done = false;
    if (done)
        return;
#ifdef YANGSCOPE_LY_LOG_CLB_V3
    ly_set_log_clb(logCallback);
#else
    ly_set_log_clb(logCallback, 1);
#endif
    ly_log_level(LY_LLWRN);
    done = true;
}

bool lessByName(const lys_module *a, const lys_module *b)
{
    int c = qstrcmp(a->name, b->name);
    if (c != 0)
        return c < 0;
    return qstrcmp(a->revision ? a->revision : "", b->revision ? b->revision : "") < 0;
}

} // namespace

YangContext::YangContext()
{
    installLoggerOnce();
    createContext();
}

YangContext::~YangContext()
{
    if (m_ctx)
        ly_ctx_destroy(m_ctx);
}

bool YangContext::createContext()
{
    if (m_ctx) {
        ly_ctx_destroy(m_ctx);
        m_ctx = nullptr;
    }
    // SET_PRIV_PARSED: every compiled node points back to its parsed node,
    // which is where typedef names, defaults and restrictions "as written" live.
    // REF_IMPLEMENTED: imported modules whose identities/leafref targets are
    // referenced get implemented too (OpenConfig relies on this heavily).
    const auto opts = LY_CTX_SET_PRIV_PARSED | LY_CTX_DISABLE_SEARCHDIR_CWD | LY_CTX_REF_IMPLEMENTED;
    QStringList sink;
    LogCapture cap(&sink);

    // Newer libyang releases load some internal modules from disk at context
    // creation. Try the compiled-in default first, then a "yang/libyang"
    // directory shipped next to the executable (portable/bundled builds),
    // then libyang's own install directory.
    QStringList internalDirs{QString()};
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        internalDirs << appDir + QStringLiteral("/yang/libyang")
                     << appDir + QStringLiteral("/../share/yangscope/yang/libyang")
                     << appDir + QStringLiteral("/../Resources/yang/libyang");
    }
#ifdef YANGSCOPE_HAVE_LY_MODULE_DIR
    internalDirs << QString::fromUtf8(ly_yang_module_dir());
#endif
    for (const QString &dir : std::as_const(internalDirs)) {
        if (!dir.isEmpty() && !QFileInfo(dir).isDir())
            continue;
        sink.clear();
        const QByteArray d = QDir::toNativeSeparators(dir).toUtf8();
        if (ly_ctx_new(dir.isEmpty() ? nullptr : d.constData(), opts, &m_ctx) == LY_SUCCESS) {
            m_lastError.clear();
            applySearchDirs();
            return true;
        }
        m_ctx = nullptr;
    }
    m_lastError = sink.isEmpty() ? QStringLiteral("could not create libyang context")
                                 : sink.join(QLatin1Char('\n'));
    return false;
}

bool YangContext::reset()
{
    return createContext();
}

void YangContext::applySearchDirs()
{
    if (!m_ctx)
        return;
    QStringList sink;
    LogCapture cap(&sink);
    for (const QString &d : std::as_const(m_searchDirs))
        ly_ctx_set_searchdir(m_ctx, QDir::toNativeSeparators(d).toUtf8().constData());
}

int YangContext::addSearchDir(const QString &dir, bool recursive)
{
    QStringList candidates;
    const QString root = QDir(dir).absolutePath();
    candidates << root;
    if (recursive) {
        QSet<QString> withYang;
        QDirIterator it(root, {QStringLiteral("*.yang")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            withYang.insert(it.fileInfo().absolutePath());
        }
        QStringList sorted(withYang.begin(), withYang.end());
        sorted.sort();
        for (const QString &d : sorted)
            if (d != root)
                candidates << d;
    }

    int added = 0;
    QStringList sink;
    LogCapture cap(&sink);
    for (const QString &d : std::as_const(candidates)) {
        if (m_searchDirs.contains(d))
            continue;
        m_searchDirs << d;
        if (m_ctx)
            ly_ctx_set_searchdir(m_ctx, QDir::toNativeSeparators(d).toUtf8().constData());
        ++added;
    }
    return added;
}

bool YangContext::isSubmoduleFile(const QString &path)
{
    if (path.endsWith(QLatin1String(".yin"), Qt::CaseInsensitive))
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QString head = QString::fromUtf8(f.read(8192));
    // Drop comments before looking at the first statement.
    static const QRegularExpression comments(QStringLiteral("//[^\\n]*|/\\*.*?\\*/"),
                                             QRegularExpression::DotMatchesEverythingOption);
    head.remove(comments);
    static const QRegularExpression first(QStringLiteral("^\\s*(sub)?module\\b"));
    const auto m = first.match(head);
    return m.hasMatch() && !m.captured(1).isEmpty();
}

QStringList YangContext::yangFilesIn(const QString &dir, bool recursive)
{
    QStringList files;
    QDirIterator it(dir, {QStringLiteral("*.yang")}, QDir::Files,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext())
        files << it.next();
    files.sort();
    return files;
}

LoadResult YangContext::loadFile(const QString &path)
{
    LoadResult r;
    if (!m_ctx) {
        r.messages << QStringLiteral("error: no libyang context");
        return r;
    }
    // Submodules are pulled in through their main module's include and can't
    // be parsed on their own; skip them quietly when a whole folder is opened.
    if (isSubmoduleFile(path)) {
        r.skipped = true;
        return r;
    }

    LogCapture cap(&r.messages);

    // Make the file's own directory available for its imports/includes.
    const QString dir = QFileInfo(path).absolutePath();
    if (!m_searchDirs.contains(dir))
        addSearchDir(dir, false);

    ly_in *in = nullptr;
    const QByteArray native = QDir::toNativeSeparators(path).toUtf8();
    if (ly_in_new_filepath(native.constData(), 0, &in) != LY_SUCCESS) {
        r.messages << QStringLiteral("error: cannot open %1").arg(path);
        return r;
    }

    const char *allFeatures[] = {"*", nullptr};
    lys_module *mod = nullptr;
    const LYS_INFORMAT fmt = path.endsWith(QLatin1String(".yin"), Qt::CaseInsensitive)
                                 ? LYS_IN_YIN : LYS_IN_YANG;
    const LY_ERR err = lys_parse(m_ctx, in, fmt, m_allFeatures ? allFeatures : nullptr, &mod);
    ly_in_free(in, 0);

    if (err == LY_SUCCESS || (err == LY_EEXIST && mod)) {
        r.module = mod;
        r.ok = true;
    } else if (r.messages.isEmpty()) {
        r.messages << QStringLiteral("error: failed to load %1 (libyang error %2)")
                          .arg(path).arg(int(err));
    }
    // libyang repeats the same warning for every node a grouping is used in.
    r.messages.removeDuplicates();
    return r;
}

LoadResult YangContext::loadModule(const QString &name, const QString &revision)
{
    LoadResult r;
    if (!m_ctx)
        return r;
    LogCapture cap(&r.messages);
    const char *allFeatures[] = {"*", nullptr};
    const QByteArray n = name.toUtf8();
    const QByteArray rev = revision.toUtf8();
    lys_module *mod = ly_ctx_load_module(m_ctx, n.constData(),
                                         revision.isEmpty() ? nullptr : rev.constData(),
                                         m_allFeatures ? allFeatures : nullptr);
    r.module = mod;
    r.ok = mod != nullptr;
    return r;
}

QVector<const lys_module *> YangContext::implementedModules(bool includeInternal) const
{
    QVector<const lys_module *> out;
    for (const lys_module *m : allModules(includeInternal))
        if (m->implemented && m->compiled)
            out << m;
    return out;
}

QVector<const lys_module *> YangContext::allModules(bool includeInternal) const
{
    QVector<const lys_module *> out;
    if (!m_ctx)
        return out;
    // libyang's own modules (ietf-yang-library, yang, ...) come first in the
    // iteration order; skip them unless asked for.
    const uint32_t internal = includeInternal ? 0 : ly_ctx_internal_modules_count(m_ctx);
    uint32_t idx = 0;
    uint32_t seen = 0;
    while (const lys_module *m = ly_ctx_get_module_iter(m_ctx, &idx)) {
        if (seen++ < internal)
            continue;
        out << m;
    }
    std::sort(out.begin(), out.end(), lessByName);
    return out;
}

QString YangContext::libyangVersion()
{
#ifdef LY_PROJ_VERSION
    return QString::fromLatin1(LY_PROJ_VERSION);
#else
    return QString::fromLatin1("so " LY_VERSION);
#endif
}

} // namespace ys
