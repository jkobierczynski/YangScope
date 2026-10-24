// SPDX-License-Identifier: GPL-3.0-or-later
// NETCONF tests: framing and XML helpers (always), and live sessions against
// tests/netconf/test_server.py (skipped when Python/paramiko are missing).
#include "core/DataTree.h"
#include "core/NetconfFraming.h"
#include "core/NetconfSession.h"
#include "core/NetconfXml.h"
#include "core/SchemaSync.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>

using namespace ys;
using namespace ys::netconf;

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                          \
        } else {                                                                 \
            ++g_passed;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        const QString a_ = (actual);                                                  \
        const QString e_ = (expected);                                                \
        if (a_ != e_) {                                                               \
            std::fprintf(stderr, "%s:%d: FAIL %s\n  got:      %s\n  expected: %s\n",  \
                         __FILE__, __LINE__, #actual, qPrintable(a_), qPrintable(e_)); \
            ++g_failed;                                                               \
        } else {                                                                      \
            ++g_passed;                                                               \
        }                                                                             \
    } while (0)

// --- unit tests -------------------------------------------------------------

static void testFraming()
{
    // end-of-message, split across feeds, two messages in one buffer
    FrameDecoder d;
    QByteArray m;
    d.feed("<a/>]]>");
    CHECK(!d.next(&m));
    d.feed("]]><b/>]]>]]>");
    CHECK(d.next(&m));
    CHECK_EQ(QString::fromUtf8(m), "<a/>");
    CHECK(d.next(&m));
    CHECK_EQ(QString::fromUtf8(m), "<b/>");
    CHECK(!d.next(&m));

    // chunked: round trip and multi-chunk message fed byte by byte
    FrameDecoder c;
    c.setChunked(true);
    const QByteArray wire = encodeFrame("<rpc-reply/>", true);
    CHECK_EQ(QString::fromLatin1(wire), "\n#12\n<rpc-reply/>\n##\n");
    const QByteArray multi = QByteArrayLiteral("\n#4\n<rpc\n#8\n-reply/>\n##\n\n#3\n<x/\n#1\n>\n##\n");
    for (char ch : multi)
        c.feed(QByteArray(1, ch));
    CHECK(c.next(&m));
    CHECK_EQ(QString::fromUtf8(m), "<rpc-reply/>");
    CHECK(c.next(&m));
    CHECK_EQ(QString::fromUtf8(m), "<x/>");
    CHECK(!c.next(&m));
    CHECK(!c.hasError());

    FrameDecoder bad;
    bad.setChunked(true);
    bad.feed("<oops/>\n##\n");
    CHECK(!bad.next(&m));
    CHECK(bad.hasError());
    FrameDecoder zero;
    zero.setChunked(true);
    zero.feed("\n#0\n\n##\n");
    CHECK(!zero.next(&m));
    CHECK(zero.hasError());
}

static void testXml()
{
    const Hello h = parseHello(
        "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"><capabilities>"
        "<capability>urn:ietf:params:netconf:base:1.1</capability>"
        "<capability>urn:ietf:params:xml:ns:yang:ietf-interfaces?module=ietf-interfaces&amp;revision=2018-02-20&amp;features=arbitrary-names,pre-provisioning</capability>"
        "</capabilities><session-id>42</session-id></hello>");
    CHECK(h.ok);
    CHECK_EQ(h.sessionId, "42");
    CHECK(h.capabilities.size() == 2);
    const ModuleCapability mc = parseModuleCapability(h.capabilities[1]);
    CHECK_EQ(mc.module, "ietf-interfaces");
    CHECK_EQ(mc.revision, "2018-02-20");
    CHECK_EQ(mc.features.join(','), "arbitrary-names,pre-provisioning");
    CHECK_EQ(shortCapabilityName(QStringLiteral("urn:ietf:params:netconf:capability:candidate:1.0")),
             ":candidate:1.0");
    CHECK(!parseHello("<foo/>").ok);

    const Reply err = parseReply(
        "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" message-id=\"7\">"
        "<rpc-error><error-type>application</error-type><error-tag>invalid-value</error-tag>"
        "<error-severity>error</error-severity><error-message xml:lang=\"en\">bad</error-message>"
        "</rpc-error></rpc-reply>");
    CHECK(err.parsed);
    CHECK_EQ(err.messageId, "7");
    CHECK(err.hasErrors());
    CHECK_EQ(err.errors.value(0).tag, "invalid-value");
    CHECK_EQ(err.errors.value(0).message, "bad");

    // Namespaces declared on rpc-reply must survive data extraction so that
    // prefixed values (identityrefs) still resolve.
    const Reply data = parseReply(
        "<nc:rpc-reply xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "xmlns:yst=\"urn:yangscope:test\" message-id=\"8\"><nc:data>"
        "<system xmlns=\"urn:yangscope:test\"><server><name>a</name><port>1</port>"
        "<proto>yst:tcp</proto></server></system></nc:data></nc:rpc-reply>");
    CHECK(data.parsed);
    CHECK(data.hasData);
    CHECK(data.data.contains("xmlns:yst=\"urn:yangscope:test\""));
    CHECK(data.data.contains("yst:tcp"));

    const Reply ok = parseReply("<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" message-id=\"9\"><ok/></rpc-reply>");
    CHECK(ok.ok && !ok.hasErrors());

    CHECK(prettyXml("<a><b>1</b></a>").contains("\n  <b>1</b>"));
    CHECK(wrapRpc("3", getConfigOp("running")).contains("message-id=\"3\""));
    CHECK(xpathFilter("/yst:system/yst:hostname", {{"yst", "urn:yangscope:test"}})
              .contains("xmlns:yst=\"urn:yangscope:test\""));

    // Device-provided identifiers must never become paths
    CHECK_EQ(schemaFileName("../../etc/passwd", "2020-01-01"), "_.._etc_passwd@2020-01-01.yang");
    CHECK_EQ(schemaFileName("ietf-ip", "2018-02-22"), "ietf-ip@2018-02-22.yang");
    CHECK_EQ(schemaFileName("a/b", ""), "a_b.yang");

    const QVector<DeviceModule> lib = parseYangLibrary(
        "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\"><module-set>"
        "<name>complete</name>"
        "<module><name>ietf-interfaces</name><revision>2018-02-20</revision>"
        "<namespace>urn:x</namespace><feature>pre-provisioning</feature>"
        "<deviation>vendor-dev</deviation></module>"
        "<import-only-module><name>ietf-yang-types</name><revision>2013-07-15</revision>"
        "<namespace>urn:y</namespace></import-only-module>"
        "</module-set></yang-library>");
    CHECK(lib.size() == 2);
    CHECK_EQ(lib.value(0).features.join(','), "pre-provisioning");
    CHECK_EQ(lib.value(0).deviations.join(','), "vendor-dev");
    CHECK(lib.value(0).implemented);
    CHECK(!lib.value(1).implemented);
}

// --- live session tests -------------------------------------------------------

struct TestServer {
    QProcess proc;
    int port = 0;
    bool start(const QStringList &extra)
    {
        const QString dir = QStringLiteral(YANGSCOPE_TEST_NETCONF);
        QStringList args{dir + QStringLiteral("/test_server.py"),
                         QStringLiteral("--port"), QStringLiteral("0"),
                         QStringLiteral("--schemas"), QStringLiteral(YANGSCOPE_TEST_DATA),
                         QStringLiteral("--schemas"), dir + QStringLiteral("/ietf"),
                         QStringLiteral("--feature"), QStringLiteral("ietf-interfaces=if-mib"),
                         QStringLiteral("--data"), dir + QStringLiteral("/data.xml"),
                         QStringLiteral("--config"), dir + QStringLiteral("/config.xml")};
        args << extra;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(QStringLiteral(YANGSCOPE_PYTHON), args);
        if (!proc.waitForStarted(5000))
            return false;
        while (proc.waitForReadyRead(10000)) {
            const QByteArray out = proc.readAll();
            const qsizetype i = out.indexOf("listening on ");
            if (i >= 0) {
                const QByteArray line = out.mid(i).split('\n').first().trimmed();
                port = line.mid(line.lastIndexOf(':') + 1).toInt();
                return port > 0;
            }
        }
        std::fprintf(stderr, "server output: %s\n", proc.readAll().constData());
        return false;
    }
    ~TestServer()
    {
        if (qEnvironmentVariableIsSet("YANGSCOPE_TEST_SERVER_LOG"))
        {
            proc.waitForReadyRead(500);
            std::fprintf(stderr, "--- server log ---\n%s\n", proc.readAll().constData());
        }
        proc.kill();
        proc.waitForFinished(2000);
    }
};

static ConnectParams params(int port, const QString &password = QStringLiteral("admin"))
{
    ConnectParams p;
    p.host = QStringLiteral("127.0.0.1");
    p.port = port;
    p.user = QStringLiteral("admin");
    p.password = password;
    p.timeoutSec = 10;
    return p;
}

static const HostKeyPrompt trustOnce = [](const QString &, const QString &fp, HostKeyStatus, bool *save) {
    *save = false; // never touch the real known_hosts from tests
    return fp.startsWith(QLatin1String("SHA256:"));
};

static QString hostnameIn(const Reply &r)
{
    const qsizetype i = r.data.indexOf("hostname>");
    if (i < 0)
        return {};
    const qsizetype start = i + 9;
    return QString::fromUtf8(r.data.mid(start, r.data.indexOf('<', start) - start));
}

static bool rpcOk(Session &s, const QByteArray &op, Reply *rep = nullptr)
{
    Reply local;
    Reply *r = rep ? rep : &local;
    if (!s.rpc(op, r))
        return false;
    if (r->hasErrors())
        std::fprintf(stderr, "  rpc-error: %s\n", qPrintable(r->errors.first().toString()));
    return !r->hasErrors();
}

static const QByteArray kSysFilter = QByteArrayLiteral(
    "<filter type=\"subtree\"><system xmlns=\"urn:yangscope:test\"/></filter>");

/// Candidate workflow: lock, edit, validate, commit, discard, confirmed
/// commit with rollback and cancel-commit.
static void testConfigWorkflow(Session &s, int port, const YangContext &ctx)
{
    Reply rep;
    if (!s.isOpen())
        std::fprintf(stderr, "  session closed before workflow: %s\n", qPrintable(s.lastError()));
    // edits to running are refused (no :writable-running)
    const bool sent = s.rpc(editConfigOp("running", "<system xmlns=\"urn:yangscope:test\"><hostname>x</hostname></system>"), &rep);
    if (!sent)
        std::fprintf(stderr, "  edit-config running: %s\n", qPrintable(s.lastError()));
    CHECK(sent);
    CHECK(rep.hasErrors() && rep.errors.value(0).tag == QLatin1String("operation-not-supported"));

    CHECK(rpcOk(s, lockOp("candidate")));
    {   // a second session cannot lock or edit
        Session other;
        CHECK(other.open(params(port), trustOnce));
        CHECK(other.rpc(lockOp("candidate"), &rep) && rep.hasErrors());
        CHECK(rep.errors.value(0).tag == QLatin1String("lock-denied"));
        CHECK(other.rpc(editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\"><mode>standalone</mode></system>"), &rep));
        CHECK(rep.hasErrors() && rep.errors.value(0).tag == QLatin1String("in-use"));
        other.close();
    }

    // edit candidate; running unchanged until commit
    CHECK(rpcOk(s, editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\"><hostname>r2</hostname></system>")));
    CHECK(rpcOk(s, getConfigOp("candidate", kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("r2"));
    CHECK(rpcOk(s, getConfigOp("running", kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("lab-router-1"));
    CHECK(rpcOk(s, validateOp("candidate")));
    CHECK(rpcOk(s, commitOp()));
    CHECK(rpcOk(s, getConfigOp("running", kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("r2"));
    // <get> reflects the running config too, plus state
    CHECK(rpcOk(s, getOp(kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("r2") && rep.data.contains("uptime"));

    // discard-changes reverts the candidate
    CHECK(rpcOk(s, editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\" xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
                                             "<server nc:operation=\"delete\"><name>ntp2</name><port>123</port></server></system>")));
    CHECK(rpcOk(s, getConfigOp("candidate", kSysFilter), &rep));
    CHECK(!rep.data.contains("ntp2"));
    CHECK(rpcOk(s, discardChangesOp()));
    CHECK(rpcOk(s, getConfigOp("candidate", kSysFilter), &rep));
    CHECK(rep.data.contains("ntp2"));

    // create with an identityref under a client-chosen prefix, then parse
    // the stored result against the schemas
    CHECK(rpcOk(s, editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\" xmlns:x=\"urn:yangscope:test\">"
                                             "<server><name>ntp3</name><port>123</port><proto>x:tcp</proto></server></system>")));
    CHECK(rpcOk(s, getConfigOp("candidate", kSysFilter), &rep));
    DataTree cand;
    CHECK(cand.parseXml(ctx.raw(), rep.data));
    CHECK(cand.toJson().contains("tcp"));
    CHECK(s.rpc(editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\" xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
                                          "<server nc:operation=\"create\"><name>ntp3</name><port>123</port></server></system>"), &rep));
    CHECK(rep.hasErrors() && rep.errors.value(0).tag == QLatin1String("data-exists"));
    CHECK(rpcOk(s, discardChangesOp()));

    // confirmed commit that is not confirmed rolls back
    CHECK(rpcOk(s, editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\"><hostname>r3</hostname></system>")));
    CHECK(rpcOk(s, commitOp(1)));
    CHECK(rpcOk(s, getConfigOp("running", kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("r3"));
    QThread::msleep(1800);
    CHECK(rpcOk(s, getConfigOp("running", kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("r2"));

    // ...and cancel-commit reverts at once
    CHECK(rpcOk(s, editConfigOp("candidate", "<system xmlns=\"urn:yangscope:test\"><hostname>r4</hostname></system>")));
    CHECK(rpcOk(s, commitOp(60)));
    CHECK(rpcOk(s, cancelCommitOp()));
    CHECK(rpcOk(s, getConfigOp("running", kSysFilter), &rep));
    CHECK(hostnameIn(rep) == QLatin1String("r2"));
    CHECK(s.rpc(cancelCommitOp(), &rep) && rep.hasErrors());

    CHECK(rpcOk(s, unlockOp("candidate")));
    CHECK(s.rpc(unlockOp("candidate"), &rep) && rep.hasErrors());

    // a lock dies with its session
    {
        Session a;
        CHECK(a.open(params(port), trustOnce));
        CHECK(rpcOk(a, lockOp("candidate")));
        a.close();
    }
    CHECK(rpcOk(s, lockOp("candidate")));
    CHECK(rpcOk(s, unlockOp("candidate")));
}

static void testLive(bool chunked)
{
    TestServer srv;
    if (!srv.start(chunked ? QStringList{} : QStringList{QStringLiteral("--base"), QStringLiteral("1.0")})) {
        CHECK(!"test server did not start");
        return;
    }

    Session s;
    CHECK(s.open(params(srv.port), trustOnce));
    if (!s.isOpen()) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(s.lastError()));
        return;
    }
    CHECK(s.chunkedFraming() == chunked);
    CHECK(!s.serverHello().sessionId.isEmpty());
    CHECK(s.hostKeyFingerprint().startsWith(QLatin1String("SHA256:")));

    // filtered get-config, parsed by libyang
    YangContext ctx;
    ctx.loadFile(QStringLiteral(YANGSCOPE_TEST_DATA "/ys-test.yang"));
    ctx.loadFile(QStringLiteral(YANGSCOPE_TEST_DATA "/ys-test-aug.yang"));
    const lysc_node *server = lys_find_path(ctx.raw(), nullptr, "/ys-test:system/server", 0);
    CHECK(server);

    Reply rep;
    QByteArray raw;
    const QByteArray filter = QByteArrayLiteral(
        "<filter type=\"subtree\"><system xmlns=\"urn:yangscope:test\"><server><name>ntp1</name>"
        "</server></system></filter>");
    CHECK(s.rpc(getConfigOp(QStringLiteral("running"), filter), &rep, &raw));
    CHECK(rep.hasData && !rep.hasErrors());
    CHECK(rep.data.contains("ntp1"));
    CHECK(!rep.data.contains("ntp2"));
    DataTree tree;
    CHECK(tree.parseXml(ctx.raw(), rep.data));
    for (const QString &m : tree.messages())
        std::fprintf(stderr, "  %s\n", qPrintable(m));
    CHECK(!tree.isEmpty());
    CHECK(tree.toJson().contains("\"ys-test:system\""));
    CHECK(tree.toJson().contains("\"ys-test-aug:comment\": \"primary\""));
    // identityref resolved (newer libyang drops the module prefix when it is
    // the leaf's own module, as RFC 7951 allows)
    CHECK(tree.toJson().contains("\"proto\": \"ys-test:udp\"")
          || tree.toJson().contains("\"proto\": \"udp\""));
    const lyd_node *sys = tree.root();
    CHECK_EQ(DataTree::name(sys), "ys-test:system");
    const lyd_node *srvNode = DataTree::firstChild(sys);
    CHECK(srvNode && DataTree::schema(srvNode) == server);
    CHECK_EQ(DataTree::path(srvNode), "/ys-test:system/server[name='ntp1'][port='123']");

    // get includes state data
    CHECK(s.rpc(getOp(QByteArrayLiteral("<filter type=\"subtree\"><system xmlns=\"urn:yangscope:test\">"
                                         "<status/></system></filter>")), &rep));
    CHECK(rep.data.contains("uptime"));

    // rpc-error is reported, session stays usable
    CHECK(s.rpc(getConfigOp(QStringLiteral("startup")), &rep));
    CHECK(rep.hasErrors());
    CHECK_EQ(rep.errors.value(0).tag, "invalid-value");
    CHECK(s.isOpen());

    // schema download
    QTemporaryDir tmp;
    const SchemaSyncResult sync = syncSchemas(s, tmp.path());
    for (const QString &m : sync.messages)
        std::fprintf(stderr, "  sync: %s\n", qPrintable(m));
    CHECK(sync.canDownload);
    CHECK_EQ(sync.moduleSource, "hello");
    CHECK(sync.downloaded.contains(QStringLiteral("ys-test@2026-10-24.yang")));
    CHECK(sync.downloaded.contains(QStringLiteral("ys-test-sub.yang")));
    CHECK(sync.failed.isEmpty());
    bool sawTest = false;
    for (const DeviceModule &m : sync.modules)
        sawTest |= m.name == QLatin1String("ys-test");
    CHECK(sawTest);
    // second run uses the cache
    const SchemaSyncResult again = syncSchemas(s, tmp.path());
    CHECK(again.downloaded.isEmpty());
    CHECK(!again.cached.isEmpty());

    // the downloaded schemas build a working context, with the advertised
    // features only (ietf-interfaces: if-mib on, arbitrary-names off)
    YangContext devCtx;
    devCtx.addSearchDir(tmp.path(), false);
    int loaded = 0;
    for (const DeviceModule &m : sync.modules)
        loaded += devCtx.loadModule(m.name, m.revision, &m.features).ok;
    CHECK(loaded == int(sync.modules.size()));
    CHECK(lys_find_path(devCtx.raw(), nullptr, "/ys-test:system/server/ys-test-aug:comment", 0));
    CHECK(lys_find_path(devCtx.raw(), nullptr, "/ietf-interfaces:interfaces/interface/if-index", 0));

    // device data parsed against the device's own schemas: identityref
    // values from another module resolve, nothing is left opaque
    CHECK(s.rpc(getOp(QByteArrayLiteral("<filter type=\"subtree\"><interfaces xmlns=\""
                                         "urn:ietf:params:xml:ns:yang:ietf-interfaces\"/></filter>")), &rep));
    DataTree ifs;
    CHECK(ifs.parseXml(devCtx.raw(), rep.data));
    CHECK(ifs.toJson().contains("\"type\": \"iana-if-type:ethernetCsmacd\""));
    CHECK(ifs.toJson().contains("\"in-octets\": \"918273645\""));
    std::function<int(const lyd_node *)> opaque = [&](const lyd_node *n) {
        int c = 0;
        for (; n; n = DataTree::next(n))
            c += DataTree::isOpaque(n) + opaque(DataTree::firstChild(n));
        return c;
    };
    CHECK(opaque(ifs.root()) == 0);

    testConfigWorkflow(s, srv.port, devCtx);

    s.close();
    CHECK(!s.isOpen());

    // wrong password
    Session bad;
    CHECK(!bad.open(params(srv.port, QStringLiteral("nope")), trustOnce));
    CHECK(bad.lastError().contains(QLatin1String("authentication")));

    // untrusted host key is refused
    Session refused;
    CHECK(!refused.open(params(srv.port),
                        [](const QString &, const QString &, HostKeyStatus, bool *) { return false; }));
    CHECK(refused.lastError().contains(QLatin1String("not trusted")));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testFraming();
    testXml();

    // Live tests need python3 + paramiko; keep HOME away from the user's
    // real ~/.ssh so known_hosts is never modified.
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    QProcess probe;
    probe.start(QStringLiteral(YANGSCOPE_PYTHON), {QStringLiteral("-c"), QStringLiteral("import paramiko")});
    const bool canLive = QStringLiteral(YANGSCOPE_PYTHON).size() > 0 && probe.waitForFinished(10000)
                         && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    if (canLive) {
        testLive(true);
        testLive(false);
    }
    std::printf("netconf: %d passed, %d failed%s\n", g_passed, g_failed,
                canLive ? "" : " (live tests skipped: python3 with paramiko not found)");
    if (g_failed)
        return 1;
    return canLive ? 0 : 77;
}
