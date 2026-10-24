// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SchemaSync.h"

#include "core/NetconfSession.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>

namespace ys::netconf {

namespace {

bool hasCapability(const QStringList &caps, const QString &prefix)
{
    for (const QString &c : caps)
        if (c.startsWith(prefix))
            return true;
    return false;
}

QString readText(QXmlStreamReader &r)
{
    return r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
}

/// Parse one <module> or <import-only-module> element (reader on its start).
DeviceModule readModule(QXmlStreamReader &r, bool implemented)
{
    DeviceModule m;
    m.implemented = implemented;
    while (r.readNextStartElement()) {
        const auto n = r.name();
        if (n == QLatin1String("name")) m.name = readText(r);
        else if (n == QLatin1String("revision")) m.revision = readText(r);
        else if (n == QLatin1String("namespace")) m.ns = readText(r);
        else if (n == QLatin1String("feature")) m.features << readText(r);
        else if (n == QLatin1String("deviation")) {
            // 1.0: <deviation><name>..</name><revision>..</revision></deviation>
            // 1.1: <deviation>module-name</deviation>
            QString dev;
            while (!r.atEnd()) {
                r.readNext();
                if (r.isStartElement() && r.name() == QLatin1String("name"))
                    dev = readText(r);
                else if (r.isCharacters() && !r.isWhitespace() && dev.isEmpty())
                    dev = r.text().toString().trimmed();
                else if (r.isEndElement() && r.name() == QLatin1String("deviation"))
                    break;
            }
            if (!dev.isEmpty())
                m.deviations << dev;
        } else if (n == QLatin1String("conformance-type")) {
            m.implemented = readText(r) == QLatin1String("implement");
        } else {
            r.skipCurrentElement();
        }
    }
    return m;
}

} // namespace

QString schemaFileName(const QString &identifier, const QString &version)
{
    // Identifiers come from the device: never let them form a path.
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9._\\-]"));
    QString id = identifier;
    id.replace(bad, QStringLiteral("_"));
    while (id.startsWith(QLatin1Char('.')))
        id.remove(0, 1);
    if (id.isEmpty())
        id = QStringLiteral("unnamed");
    QString ver = version;
    ver.replace(bad, QStringLiteral("_"));
    return ver.isEmpty() ? id + QStringLiteral(".yang")
                         : id + QLatin1Char('@') + ver + QStringLiteral(".yang");
}

QVector<DeviceModule> modulesFromHello(const QStringList &capabilities)
{
    QVector<DeviceModule> out;
    for (const ModuleCapability &c : moduleCapabilities(capabilities)) {
        DeviceModule m;
        m.name = c.module;
        m.revision = c.revision;
        m.ns = c.ns;
        m.features = c.features;
        m.deviations = c.deviations;
        out << m;
    }
    return out;
}

QVector<DeviceModule> parseYangLibrary(const QByteArray &data)
{
    // Handles both RFC 7895 (/modules-state/module) and RFC 8525
    // (/yang-library/module-set/module and import-only-module).
    QVector<DeviceModule> out;
    QXmlStreamReader r(data);
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement() || r.namespaceUri() != kYangLibraryNs)
            continue;
        const auto n = r.name();
        if (n == QLatin1String("module"))
            out << readModule(r, true);
        else if (n == QLatin1String("import-only-module"))
            out << readModule(r, false);
    }
    // de-duplicate (a module can appear in several module-sets)
    QSet<QString> seen;
    QVector<DeviceModule> unique;
    for (const DeviceModule &m : out) {
        const QString key = m.name + QLatin1Char('@') + m.revision;
        if (!m.name.isEmpty() && !seen.contains(key)) {
            seen.insert(key);
            unique << m;
        }
    }
    return unique;
}

SchemaSyncResult syncSchemas(Session &session, const QString &cacheDir,
                             const SyncProgress &progress)
{
    SchemaSyncResult res;
    res.cacheDir = cacheDir;
    const QStringList caps = session.serverHello().capabilities;

    // 1. Which modules does the device implement?
    if (hasCapability(caps, kYangLibrary11)) {
        Reply rep;
        const QByteArray filter = QByteArrayLiteral("<filter type=\"subtree\"><yang-library xmlns=\"")
                                  + kYangLibraryNs.toUtf8()
                                  + QByteArrayLiteral("\"><module-set/></yang-library></filter>");
        if (session.rpc(getOp(filter), &rep) && !rep.hasErrors()) {
            res.modules = parseYangLibrary(rep.data);
            res.moduleSource = QStringLiteral("yang-library 1.1");
        } else {
            res.messages << QStringLiteral("yang-library query failed: ")
                                + (rep.errors.isEmpty() ? session.lastError() : rep.errors.first().toString());
        }
    }
    if (res.modules.isEmpty() && hasCapability(caps, kYangLibrary10)) {
        Reply rep;
        const QByteArray filter = QByteArrayLiteral("<filter type=\"subtree\"><modules-state xmlns=\"")
                                  + kYangLibraryNs.toUtf8()
                                  + QByteArrayLiteral("\"/></filter>");
        if (session.rpc(getOp(filter), &rep) && !rep.hasErrors()) {
            res.modules = parseYangLibrary(rep.data);
            res.moduleSource = QStringLiteral("yang-library 1.0");
        }
    }
    if (res.modules.isEmpty()) {
        res.modules = modulesFromHello(caps);
        res.moduleSource = QStringLiteral("hello");
    }
    if (!session.isOpen()) {
        res.messages << session.lastError();
        return res;
    }

    // 2. Which schema files can be downloaded?
    QVector<SchemaEntry> schemas;
    res.canDownload = hasCapability(caps, kMonitoringNs);
    if (res.canDownload) {
        Reply rep;
        if (session.rpc(getOp(monitoringSchemasFilter()), &rep) && !rep.hasErrors()) {
            for (const SchemaEntry &e : parseMonitoringSchemas(rep.data))
                if (e.format.endsWith(QLatin1String("yang")) && !e.format.endsWith(QLatin1String("yin")))
                    schemas << e;
        } else {
            res.messages << QStringLiteral("could not list schemas (ietf-netconf-monitoring): ")
                                + (rep.errors.isEmpty() ? session.lastError() : rep.errors.first().toString());
        }
    }
    if (schemas.isEmpty()) {
        // No schema list: still try get-schema for each module we know about.
        for (const DeviceModule &m : std::as_const(res.modules))
            schemas << SchemaEntry{m.name, m.revision, QStringLiteral("yang"), m.ns};
    }

    // 3. Download what is not cached yet.
    QDir dir(cacheDir);
    if (!dir.mkpath(QStringLiteral("."))) {
        res.messages << QStringLiteral("cannot create %1").arg(cacheDir);
        return res;
    }
    int done = 0;
    for (const SchemaEntry &e : std::as_const(schemas)) {
        const QString file = schemaFileName(e.identifier, e.version);
        if (progress && !progress(done, int(schemas.size()), e.identifier)) {
            res.messages << QStringLiteral("cancelled");
            break;
        }
        ++done;
        if (QFileInfo::exists(dir.filePath(file))) {
            res.cached << file;
            continue;
        }
        if (!res.canDownload) {
            res.failed << e.identifier;
            continue;
        }
        Reply rep;
        if (!session.rpc(getSchemaOp(e.identifier, e.version), &rep, nullptr, 60000)) {
            res.failed << e.identifier;
            res.messages << QStringLiteral("%1: %2").arg(e.identifier, session.lastError());
            if (!session.isOpen())
                break;
            continue;
        }
        if (rep.hasErrors() || rep.dataText.trimmed().isEmpty()) {
            res.failed << e.identifier;
            res.messages << QStringLiteral("%1: %2").arg(
                e.identifier, rep.errors.isEmpty() ? QStringLiteral("empty schema")
                                                   : rep.errors.first().toString());
            continue;
        }
        QSaveFile f(dir.filePath(file));
        if (!f.open(QIODevice::WriteOnly) || f.write(rep.dataText.toUtf8()) < 0 || !f.commit()) {
            res.failed << e.identifier;
            res.messages << QStringLiteral("%1: cannot write %2").arg(e.identifier, f.fileName());
            continue;
        }
        res.downloaded << file;
    }
    if (progress)
        progress(done, int(schemas.size()), QString());
    return res;
}

} // namespace ys::netconf
