// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NetconfXml.h"

#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace ys::netconf {

namespace {

QString esc(const QString &s) { return s.toHtmlEscaped(); } // also escapes quotes

/// Copy the current element (reader positioned on its StartElement) and all
/// of its content to the writer, keeping namespaces. @p inherited holds
/// declarations made on ancestors outside the copied subtree (rpc-reply,
/// data); they are repeated on the copied root.
void copyElement(QXmlStreamReader &r, QXmlStreamWriter &w,
                 const QXmlStreamNamespaceDeclarations &inherited)
{
    int depth = 0;
    do {
        switch (r.tokenType()) {
        case QXmlStreamReader::StartElement: {
            // Declarations go first so the writer uses them for this element's
            // own prefix. Every source declaration is kept, so prefixes used
            // inside values (identityrefs, instance-identifiers) still resolve.
            auto declare = [&w](const QXmlStreamNamespaceDeclaration &d) {
                if (d.prefix().isEmpty())
                    w.writeDefaultNamespace(d.namespaceUri().toString());
                else
                    w.writeNamespace(d.namespaceUri().toString(), d.prefix().toString());
            };
            if (depth == 0)
                for (const auto &d : inherited)
                    if (!d.prefix().isEmpty())
                        declare(d);
            for (const auto &d : r.namespaceDeclarations())
                declare(d);
            ++depth;
            w.writeStartElement(r.namespaceUri().toString(), r.name().toString());
            for (const QXmlStreamAttribute &a : r.attributes()) {
                if (a.namespaceUri().isEmpty())
                    w.writeAttribute(a.name().toString(), a.value().toString());
                else
                    w.writeAttribute(a.namespaceUri().toString(), a.name().toString(),
                                     a.value().toString());
            }
            break;
        }
        case QXmlStreamReader::EndElement:
            --depth;
            w.writeEndElement();
            break;
        case QXmlStreamReader::Characters:
            if (!r.isWhitespace())
                w.writeCharacters(r.text().toString());
            break;
        default:
            break;
        }
        if (depth == 0)
            return;
        r.readNext();
    } while (!r.atEnd());
}

QString readText(QXmlStreamReader &r)
{
    return r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
}

} // namespace

QString RpcError::toString() const
{
    QString s = severity.isEmpty() ? QStringLiteral("error") : severity;
    s += QStringLiteral(": ") + (tag.isEmpty() ? QStringLiteral("rpc-error") : tag);
    if (!message.isEmpty())
        s += QStringLiteral(" - ") + message;
    if (!path.isEmpty())
        s += QStringLiteral(" (path ") + path + QLatin1Char(')');
    if (!appTag.isEmpty())
        s += QStringLiteral(" [app-tag ") + appTag + QLatin1Char(']');
    return s;
}

bool Reply::hasErrors() const
{
    for (const RpcError &e : errors)
        if (e.severity != QLatin1String("warning"))
            return true;
    return false;
}

QByteArray clientHello()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"><capabilities>"
        "<capability>urn:ietf:params:netconf:base:1.0</capability>"
        "<capability>urn:ietf:params:netconf:base:1.1</capability>"
        "</capabilities></hello>");
}

Hello parseHello(const QByteArray &xml)
{
    Hello h;
    QXmlStreamReader r(xml);
    bool inHello = false;
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement())
            continue;
        const auto name = r.name();
        if (name == QLatin1String("hello") && r.namespaceUri() == kBaseNs)
            inHello = true;
        else if (inHello && name == QLatin1String("capability"))
            h.capabilities << readText(r);
        else if (inHello && name == QLatin1String("session-id"))
            h.sessionId = readText(r);
    }
    if (r.hasError())
        h.error = QStringLiteral("malformed hello: ") + r.errorString();
    else if (!inHello)
        h.error = QStringLiteral("server did not send a NETCONF hello");
    else if (h.capabilities.isEmpty())
        h.error = QStringLiteral("server hello has no capabilities");
    h.ok = h.error.isEmpty();
    return h;
}

QByteArray wrapRpc(const QString &messageId, const QByteArray &operation)
{
    return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                             "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" message-id=\"")
           + esc(messageId).toUtf8() + QByteArrayLiteral("\">") + operation
           + QByteArrayLiteral("</rpc>");
}

Reply parseReply(const QByteArray &xml)
{
    Reply rep;
    QXmlStreamReader r(xml);
    QXmlStreamNamespaceDeclarations inherited;
    int depth = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement()) {
            --depth;
            continue;
        }
        if (!r.isStartElement())
            continue;
        ++depth;
        const auto name = r.name();
        if (depth == 1) {
            if (name != QLatin1String("rpc-reply") || r.namespaceUri() != kBaseNs) {
                rep.parseError = QStringLiteral("not an rpc-reply: <%1>").arg(name.toString());
                return rep;
            }
            rep.messageId = r.attributes().value(QLatin1String("message-id")).toString();
            inherited = r.namespaceDeclarations();
            rep.parsed = true;
            continue;
        }
        if (depth != 2)
            continue;
        if (name == QLatin1String("ok")) {
            rep.ok = true;
        } else if (name == QLatin1String("rpc-error")) {
            RpcError e;
            while (r.readNextStartElement()) {
                const auto n = r.name();
                if (n == QLatin1String("error-type")) e.type = readText(r);
                else if (n == QLatin1String("error-tag")) e.tag = readText(r);
                else if (n == QLatin1String("error-severity")) e.severity = readText(r);
                else if (n == QLatin1String("error-app-tag")) e.appTag = readText(r);
                else if (n == QLatin1String("error-path")) e.path = readText(r);
                else if (n == QLatin1String("error-message")) e.message = readText(r);
                else r.skipCurrentElement();
            }
            rep.errors << e;
            --depth; // readNextStartElement consumed our end element
        } else if (name == QLatin1String("data")) {
            rep.hasData = true;
            inherited += r.namespaceDeclarations();
            QByteArray out;
            QXmlStreamWriter w(&out);
            w.setAutoFormatting(false);
            QString text;
            r.readNext();
            while (!r.atEnd() && !(r.isEndElement() && r.name() == QLatin1String("data"))) {
                if (r.isStartElement()) {
                    copyElement(r, w, inherited);
                } else if (r.isCharacters()) {
                    text += r.text();
                }
                r.readNext();
            }
            --depth;
            rep.data = out;
            rep.dataText = text;
        } else {
            r.skipCurrentElement();
            --depth;
        }
    }
    if (r.hasError() && rep.parseError.isEmpty())
        rep.parseError = QStringLiteral("malformed reply: ") + r.errorString();
    return rep;
}

ModuleCapability parseModuleCapability(const QString &capability)
{
    ModuleCapability m;
    const qsizetype q = capability.indexOf(QLatin1Char('?'));
    if (q < 0)
        return m;
    m.ns = capability.left(q);
    // The query uses '&' (XML-unescaped already) separated key=value pairs.
    QUrlQuery query(capability.mid(q + 1));
    m.module = query.queryItemValue(QStringLiteral("module"), QUrl::FullyDecoded);
    m.revision = query.queryItemValue(QStringLiteral("revision"), QUrl::FullyDecoded);
    const QString f = query.queryItemValue(QStringLiteral("features"), QUrl::FullyDecoded);
    const QString d = query.queryItemValue(QStringLiteral("deviations"), QUrl::FullyDecoded);
    if (!f.isEmpty())
        m.features = f.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (!d.isEmpty())
        m.deviations = d.split(QLatin1Char(','), Qt::SkipEmptyParts);
    return m;
}

QVector<ModuleCapability> moduleCapabilities(const QStringList &capabilities)
{
    QVector<ModuleCapability> out;
    for (const QString &c : capabilities) {
        ModuleCapability m = parseModuleCapability(c);
        if (m.valid())
            out << m;
    }
    return out;
}

QString shortCapabilityName(const QString &capability)
{
    static const QString prefix = QStringLiteral("urn:ietf:params:netconf:capability:");
    static const QString base = QStringLiteral("urn:ietf:params:netconf:base:");
    QString c = capability.section(QLatin1Char('?'), 0, 0);
    if (c.startsWith(prefix))
        return QLatin1Char(':') + c.mid(prefix.size());
    if (c.startsWith(base))
        return QStringLiteral("base:") + c.mid(base.size());
    return {};
}

QVector<SchemaEntry> parseMonitoringSchemas(const QByteArray &data)
{
    QVector<SchemaEntry> out;
    QXmlStreamReader r(data);
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement() || r.name() != QLatin1String("schema")
            || r.namespaceUri() != kMonitoringNs)
            continue;
        SchemaEntry e;
        while (r.readNextStartElement()) {
            const auto n = r.name();
            if (n == QLatin1String("identifier")) e.identifier = readText(r);
            else if (n == QLatin1String("version")) e.version = readText(r);
            else if (n == QLatin1String("format")) e.format = readText(r);
            else if (n == QLatin1String("namespace")) e.ns = readText(r);
            else r.skipCurrentElement();
        }
        // format is an identityref; accept "yang", "ncm:yang", "xxx:yang"
        if (!e.identifier.isEmpty())
            out << e;
    }
    return out;
}

QByteArray getOp(const QByteArray &filter)
{
    return QByteArrayLiteral("<get>") + filter + QByteArrayLiteral("</get>");
}

QByteArray getConfigOp(const QString &datastore, const QByteArray &filter)
{
    return QByteArrayLiteral("<get-config><source><") + esc(datastore).toUtf8()
           + QByteArrayLiteral("/></source>") + filter + QByteArrayLiteral("</get-config>");
}

QByteArray getSchemaOp(const QString &identifier, const QString &version, const QString &format)
{
    QByteArray op = QByteArrayLiteral("<get-schema xmlns=\"") + kMonitoringNs.toUtf8()
                    + QByteArrayLiteral("\"><identifier>") + esc(identifier).toUtf8()
                    + QByteArrayLiteral("</identifier>");
    if (!version.isEmpty())
        op += QByteArrayLiteral("<version>") + esc(version).toUtf8() + QByteArrayLiteral("</version>");
    op += QByteArrayLiteral("<format>") + esc(format).toUtf8()
          + QByteArrayLiteral("</format></get-schema>");
    return op;
}

QByteArray closeSessionOp()
{
    return QByteArrayLiteral("<close-session/>");
}

QByteArray editConfigOp(const QString &target, const QByteArray &configContent,
                        const QString &defaultOperation, TestOption test, ErrorOption error)
{
    QByteArray op = QByteArrayLiteral("<edit-config><target><") + esc(target).toUtf8()
                    + QByteArrayLiteral("/></target>");
    if (!defaultOperation.isEmpty())
        op += QByteArrayLiteral("<default-operation>") + esc(defaultOperation).toUtf8()
              + QByteArrayLiteral("</default-operation>");
    switch (test) {
    case TestOption::TestThenSet: op += "<test-option>test-then-set</test-option>"; break;
    case TestOption::Set: op += "<test-option>set</test-option>"; break;
    case TestOption::TestOnly: op += "<test-option>test-only</test-option>"; break;
    case TestOption::Default: break;
    }
    switch (error) {
    case ErrorOption::StopOnError: op += "<error-option>stop-on-error</error-option>"; break;
    case ErrorOption::ContinueOnError: op += "<error-option>continue-on-error</error-option>"; break;
    case ErrorOption::RollbackOnError: op += "<error-option>rollback-on-error</error-option>"; break;
    case ErrorOption::Default: break;
    }
    op += QByteArrayLiteral("<config xmlns:nc=\"") + kBaseNs.toUtf8() + QByteArrayLiteral("\">")
          + configContent + QByteArrayLiteral("</config></edit-config>");
    return op;
}

QByteArray lockOp(const QString &datastore)
{
    return QByteArrayLiteral("<lock><target><") + esc(datastore).toUtf8()
           + QByteArrayLiteral("/></target></lock>");
}

QByteArray unlockOp(const QString &datastore)
{
    return QByteArrayLiteral("<unlock><target><") + esc(datastore).toUtf8()
           + QByteArrayLiteral("/></target></unlock>");
}

QByteArray validateOp(const QString &source)
{
    return QByteArrayLiteral("<validate><source><") + esc(source).toUtf8()
           + QByteArrayLiteral("/></source></validate>");
}

QByteArray discardChangesOp()
{
    return QByteArrayLiteral("<discard-changes/>");
}

QByteArray commitOp(int confirmTimeoutSec, const QString &persist)
{
    if (confirmTimeoutSec <= 0 && persist.isEmpty())
        return QByteArrayLiteral("<commit/>");
    QByteArray op = QByteArrayLiteral("<commit><confirmed/>");
    if (confirmTimeoutSec > 0)
        op += QByteArrayLiteral("<confirm-timeout>") + QByteArray::number(confirmTimeoutSec)
              + QByteArrayLiteral("</confirm-timeout>");
    if (!persist.isEmpty())
        op += QByteArrayLiteral("<persist>") + esc(persist).toUtf8() + QByteArrayLiteral("</persist>");
    return op + QByteArrayLiteral("</commit>");
}

QByteArray cancelCommitOp(const QString &persistId)
{
    if (persistId.isEmpty())
        return QByteArrayLiteral("<cancel-commit/>");
    return QByteArrayLiteral("<cancel-commit><persist-id>") + esc(persistId).toUtf8()
           + QByteArrayLiteral("</persist-id></cancel-commit>");
}

QByteArray monitoringSchemasFilter()
{
    return QByteArrayLiteral("<filter type=\"subtree\"><netconf-state xmlns=\"")
           + kMonitoringNs.toUtf8() + QByteArrayLiteral("\"><schemas/></netconf-state></filter>");
}

QByteArray xpathFilter(const QString &select, const QVector<QPair<QString, QString>> &prefixes)
{
    QByteArray f = QByteArrayLiteral("<filter type=\"xpath\"");
    for (const auto &p : prefixes)
        f += QByteArrayLiteral(" xmlns:") + p.first.toUtf8() + QByteArrayLiteral("=\"")
             + esc(p.second).toUtf8() + '"';
    f += QByteArrayLiteral(" select=\"") + esc(select).toUtf8() + QByteArrayLiteral("\"/>");
    return f;
}

QString prettyXml(const QByteArray &xml)
{
    QXmlStreamReader r(xml);
    QString out;
    QXmlStreamWriter w(&out);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);
    while (!r.atEnd()) {
        r.readNext();
        switch (r.tokenType()) {
        case QXmlStreamReader::StartElement:
            w.writeStartElement(r.qualifiedName().toString());
            for (const QXmlStreamNamespaceDeclaration &d : r.namespaceDeclarations()) {
                if (d.prefix().isEmpty())
                    w.writeAttribute(QStringLiteral("xmlns"), d.namespaceUri().toString());
                else
                    w.writeAttribute(QStringLiteral("xmlns:") + d.prefix().toString(),
                                     d.namespaceUri().toString());
            }
            for (const QXmlStreamAttribute &a : r.attributes())
                w.writeAttribute(a.qualifiedName().toString(), a.value().toString());
            break;
        case QXmlStreamReader::EndElement:
            w.writeEndElement();
            break;
        case QXmlStreamReader::Characters:
            if (!r.isWhitespace())
                w.writeCharacters(r.text().toString());
            break;
        case QXmlStreamReader::Comment:
            w.writeComment(r.text().toString());
            break;
        default:
            break;
        }
    }
    if (r.hasError())
        return QString::fromUtf8(xml);
    return out.trimmed();
}

} // namespace ys::netconf
