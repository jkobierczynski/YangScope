// SPDX-License-Identifier: GPL-3.0-or-later
// XML helpers for NETCONF messages (RFC 6241) and schema discovery
// (RFC 6020 module capabilities, RFC 6022 monitoring). Qt only, no libyang.
#pragma once

#include <QMetaType>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ys::netconf {

inline const QString kBaseNs = QStringLiteral("urn:ietf:params:xml:ns:netconf:base:1.0");
inline const QString kMonitoringNs = QStringLiteral("urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring");
inline const QString kBase10 = QStringLiteral("urn:ietf:params:netconf:base:1.0");
inline const QString kBase11 = QStringLiteral("urn:ietf:params:netconf:base:1.1");

struct Hello {
    QStringList capabilities;
    QString sessionId;
    bool ok = false;
    QString error;
};

struct RpcError {
    QString type, tag, severity, appTag, path, message;
    QString toString() const;
};

struct Reply {
    QString messageId;
    bool ok = false;           ///< <ok/> present
    bool parsed = false;       ///< XML was well-formed rpc-reply
    QVector<RpcError> errors;
    bool hasData = false;
    QByteArray data;           ///< children of <data>, re-serialised with namespaces
    QString dataText;          ///< text content of <data> (get-schema)
    QString parseError;
    bool hasErrors() const;    ///< any error with severity "error"
};

/// A YANG module advertised in a hello capability (RFC 6020 section 5.6.4).
struct ModuleCapability {
    QString ns, module, revision;
    QStringList features, deviations;
    bool valid() const { return !module.isEmpty(); }
};

/// One entry of /netconf-state/schemas (RFC 6022).
struct SchemaEntry {
    QString identifier, version, format, ns;
};

QByteArray clientHello();
Hello parseHello(const QByteArray &xml);
/// Wrap an operation element in <rpc message-id="...">.
QByteArray wrapRpc(const QString &messageId, const QByteArray &operation);
Reply parseReply(const QByteArray &xml);

ModuleCapability parseModuleCapability(const QString &capability);
QVector<ModuleCapability> moduleCapabilities(const QStringList &capabilities);
/// "urn:ietf:params:netconf:capability:xpath:1.0" -> ":xpath:1.0" style short name, or empty.
QString shortCapabilityName(const QString &capability);

/// Parse the <netconf-state><schemas> part of a get reply's data.
QVector<SchemaEntry> parseMonitoringSchemas(const QByteArray &data);

// Operation builders -----------------------------------------------------------
QByteArray getOp(const QByteArray &filter = {});
QByteArray getConfigOp(const QString &datastore, const QByteArray &filter = {});
QByteArray getSchemaOp(const QString &identifier, const QString &version,
                       const QString &format = QStringLiteral("yang"));
QByteArray closeSessionOp();

enum class TestOption { Default, TestThenSet, Set, TestOnly };
enum class ErrorOption { Default, StopOnError, ContinueOnError, RollbackOnError };
/// <edit-config>; @p configContent are the children of <config>, which may
/// use the "nc" prefix for nc:operation (it is declared on <config>).
/// @p defaultOperation: empty, "merge", "replace" or "none".
QByteArray editConfigOp(const QString &target, const QByteArray &configContent,
                        const QString &defaultOperation = {},
                        TestOption test = TestOption::Default,
                        ErrorOption error = ErrorOption::Default);
QByteArray lockOp(const QString &datastore);
QByteArray unlockOp(const QString &datastore);
QByteArray validateOp(const QString &source);
QByteArray discardChangesOp();
/// <commit>; with @p confirmTimeoutSec > 0 a confirmed commit (RFC 6241
/// 8.4) that the device rolls back unless confirmed in time. @p persist is
/// the optional persist-id (base:1.1) so another session can confirm it.
QByteArray commitOp(int confirmTimeoutSec = 0, const QString &persist = {});
QByteArray cancelCommitOp(const QString &persistId = {});
QByteArray monitoringSchemasFilter();
/// <filter type="xpath" select="..."> with namespace declarations for the
/// given prefix -> namespace pairs.
QByteArray xpathFilter(const QString &select, const QVector<QPair<QString, QString>> &prefixes);

/// Indent XML for display. Returns the input unchanged if it is not well-formed.
QString prettyXml(const QByteArray &xml);

} // namespace ys::netconf

Q_DECLARE_METATYPE(ys::netconf::Reply)
