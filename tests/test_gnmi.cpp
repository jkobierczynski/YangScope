// SPDX-License-Identifier: GPL-3.0-or-later
// Live gNMI client tests against tests/gnmi/test_server.py. Skipped (77) when
// python3 with grpcio and grpcio-tools is not available.
#include "core/DataTree.h"
#include "core/GnmiClient.h"
#include "core/GnmiData.h"
#include "core/YangContext.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace ys;
using namespace ys::gnmi;

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

struct Server {
    QProcess proc;
    int port = 0;
    bool start(const QStringList &extra)
    {
        QStringList args{QStringLiteral(YANGSCOPE_TEST_GNMI "/test_server.py"), QStringLiteral("--port"),
                         QStringLiteral("0"), QStringLiteral("--tick"), QStringLiteral("0.2")};
        args << extra;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(QStringLiteral(YANGSCOPE_PYTHON), args);
        if (!proc.waitForStarted(5000))
            return false;
        QByteArray all;
        while (proc.waitForReadyRead(30000)) {
            all += proc.readAll();
            const qsizetype i = all.indexOf("listening on ");
            if (i >= 0) {
                QByteArray line = all.mid(i).split('\n').first().trimmed();
                line = line.split(' ').value(2);
                port = line.mid(line.lastIndexOf(':') + 1).toInt();
                return port > 0;
            }
        }
        std::fprintf(stderr, "server output: %s\n", all.constData());
        return false;
    }
    ~Server()
    {
        proc.kill();
        proc.waitForFinished(2000);
    }
};

static ConnectParams insecure(int port)
{
    ConnectParams p;
    p.host = QStringLiteral("127.0.0.1");
    p.port = port;
    p.tls = TlsMode::Insecure;
    p.username = QStringLiteral("admin");
    p.password = QStringLiteral("admin");
    p.timeoutSec = 5;
    return p;
}

static Path P(const char *s)
{
    Path p;
    parsePath(QString::fromUtf8(s), &p);
    return p;
}

static void testPlain(const YangContext &ctx)
{
    Server srv;
    CHECK(srv.start({QStringLiteral("--user"), QStringLiteral("admin"), QStringLiteral("--password"),
                     QStringLiteral("admin")}));
    if (!srv.port)
        return;
    Client c;
    CHECK(c.connect(insecure(srv.port)));
    if (!c.isConnected()) {
        std::fprintf(stderr, "connect: %s\n", qPrintable(c.lastError()));
        return;
    }

    Capabilities caps;
    CHECK(c.capabilities(&caps));
    CHECK(caps.version == QLatin1String("0.10.0"));
    CHECK(caps.models.size() == 5);
    CHECK(caps.encodings.contains(Encoding::JsonIetf));

    // Get a container as JSON_IETF and place it in a libyang tree
    QVector<Notification> notes;
    CHECK(c.get(Path(), {P("/interfaces/interface[name=eth0]")}, DataType::All, Encoding::JsonIetf, &notes));
    CHECK(notes.size() == 1 && notes[0].updates.size() == 1);
    DataTree tree;
    QStringList msgs;
    CHECK(notificationsToTree(ctx.raw(), notes, &tree, &msgs) == 1);
    for (const QString &m : msgs)
        std::fprintf(stderr, "  %s\n", qPrintable(m));
    const QString js = tree.toJson();
    CHECK(js.contains("\"in-octets\""));
    CHECK(js.contains("\"ietf-ip:ipv4\""));
    CHECK(js.contains("iana-if-type:ethernetCsmacd"));

    // Wildcard keys, PROTO encoding: one update per entry with concrete keys
    CHECK(c.get(Path(), {P("/system/server[name=*][port=*]/weight")}, DataType::Config, Encoding::Proto, &notes));
    CHECK(notes.size() == 1 && notes[0].updates.size() == 2);
    if (!notes.isEmpty() && notes[0].updates.size() == 2) {
        CHECK(notes[0].updates[0].value.kind == Value::String);
        CHECK(pathToString(notes[0].updates[0].path) == QLatin1String("/ys-test:system/server[name=ntp1][port=123]/weight"));
    }
    DataTree weights;
    msgs.clear();
    CHECK(notificationsToTree(ctx.raw(), notes, &weights, &msgs) == 2);

    // NOT_FOUND is reported
    CHECK(!c.get(Path(), {P("/system/nothing")}, DataType::All, Encoding::JsonIetf, &notes));
    CHECK(c.lastError().startsWith(QLatin1String("NOT_FOUND")));

    // Set: validate locally first, then send
    DataTree cfg;
    CHECK(c.get(Path(), {P("/system")}, DataType::Config, Encoding::JsonIetf, &notes));
    msgs.clear();
    CHECK(notificationsToTree(ctx.raw(), notes, &cfg, &msgs) == 1);
    QVector<SetOperation> ops;
    QStringList errs;
    CHECK(parseSetScript("update /system/hostname = r2\n"
                         "replace /system/server[name=ntp1][port=123] = {\"weight\": \"75.00\", \"proto\": \"ys-test:tcp\"}\n"
                         "delete /system/server[name=ntp2][port=123]\n", &ops, &errs));
    const EditValidation v = validateSet(ctx.raw(), ops, &cfg);
    CHECK(v.ok && v.fullValidation);
    for (const QString &e : v.errors)
        std::fprintf(stderr, "  validate: %s\n", qPrintable(e));
    CHECK(v.changes.size() >= 4);
    SetResult res;
    CHECK(c.set(ops, &res));
    CHECK(res.results.size() == 3);
    CHECK(res.results.value(0).startsWith(QLatin1String("DELETE")));
    // check on the device
    CHECK(c.get(Path(), {P("/system")}, DataType::Config, Encoding::JsonIetf, &notes));
    DataTree after;
    msgs.clear();
    CHECK(notificationsToTree(ctx.raw(), notes, &after, &msgs) == 1);
    const QString aj = after.toJson();
    CHECK(aj.contains("\"hostname\": \"r2\""));
    CHECK(!aj.contains("ntp2"));
    CHECK(aj.contains("75.0") && !aj.contains("192.0.2.10"));

    // Subscribe ONCE: updates, sync, end
    {
        SubscribeSpec spec;
        spec.mode = ListMode::Once;
        spec.subscriptions = {SubscriptionSpec{P("/interfaces/interface[name=eth0]/statistics")}};
        int updates = 0, syncs = 0;
        CHECK(c.subscribe(spec, [&](const Notification &n) { updates += int(n.updates.size()); },
                          [&] { ++syncs; }));
        CHECK(updates >= 5 && syncs == 1);
    }
    // STREAM SAMPLE with cancel from another thread; counters grow
    {
        SubscribeSpec spec;
        spec.mode = ListMode::Stream;
        SubscriptionSpec s{P("/interfaces/interface[name=eth0]/statistics/in-octets")};
        s.mode = SubscriptionMode::Sample;
        s.sampleIntervalNs = 300'000'000;
        spec.subscriptions = {s};
        std::atomic<int> updates{0};
        QStringList values;
        std::mutex m;
        std::thread stopper([&] {
            for (int i = 0; i < 100 && updates < 4; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            c.cancelSubscription();
        });
        const bool ok = c.subscribe(spec, [&](const Notification &n) {
            std::lock_guard<std::mutex> l(m);
            for (const Update &u : n.updates)
                values << u.value.text;
            ++updates;
        }, {});
        stopper.join();
        CHECK(ok);
        CHECK(updates >= 4);
        CHECK(values.size() >= 4 && values.first() != values.last());
    }
    // POLL: each poll gives a new snapshot
    {
        SubscribeSpec spec;
        spec.mode = ListMode::Poll;
        spec.subscriptions = {SubscriptionSpec{P("/system/hostname")}};
        std::atomic<int> syncs{0};
        std::thread poller([&] {
            for (int i = 0; i < 50 && syncs < 1; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            c.poll();
            for (int i = 0; i < 50 && syncs < 2; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            c.poll();
            for (int i = 0; i < 50 && syncs < 3; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            c.cancelSubscription();
        });
        int updates = 0;
        CHECK(c.subscribe(spec, [&](const Notification &) { ++updates; }, [&] { ++syncs; }));
        poller.join();
        CHECK(syncs == 3 && updates == 3);
    }

    // wrong password
    Client bad;
    ConnectParams bp = insecure(srv.port);
    bp.password = QStringLiteral("nope");
    CHECK(bad.connect(bp)); // the channel itself connects
    CHECK(!bad.capabilities(&caps));
    CHECK(bad.lastError().startsWith(QLatin1String("UNAUTHENTICATED")));
}

static void testTls()
{
    QTemporaryDir certs;
    Server srv;
    CHECK(srv.start({QStringLiteral("--tls"), certs.path()}));
    if (!srv.port)
        return;
    ConnectParams p;
    p.host = QStringLiteral("127.0.0.1");
    p.port = srv.port;
    p.timeoutSec = 4;
    p.tls = TlsMode::Tls;
    p.caFile = certs.filePath(QStringLiteral("ca.pem"));
    Capabilities caps;
    {
        Client c;
        CHECK(c.connect(p));
        CHECK(c.capabilities(&caps));
    }
    {   // server name override: certificate also valid for router.lab
        Client c;
        ConnectParams o = p;
        o.serverName = QStringLiteral("router.lab");
        CHECK(c.connect(o) && c.capabilities(&caps));
        o.serverName = QStringLiteral("other.lab");
        Client d;
        CHECK(!d.connect(o));
    }
    {   // not trusted without the CA
        Client c;
        ConnectParams o = p;
        o.caFile.clear();
        CHECK(!c.connect(o));
    }
    {   // plain text against a TLS server
        Client c;
        ConnectParams o = p;
        o.tls = TlsMode::Insecure;
        CHECK(!c.connect(o) || !c.capabilities(&caps));
    }
    {
        Client c;
        ConnectParams o = p;
        o.caFile = certs.filePath(QStringLiteral("missing.pem"));
        CHECK(!c.connect(o) && c.lastError().contains(QLatin1String("cannot read")));
    }

    // mutual TLS
    Server m;
    CHECK(m.start({QStringLiteral("--tls"), certs.path(), QStringLiteral("--mtls")}));
    if (!m.port)
        return;
    p.port = m.port;
    {
        Client c;
        CHECK(!c.connect(p) || !c.capabilities(&caps));
    }
    {
        Client c;
        ConnectParams o = p;
        o.certFile = certs.filePath(QStringLiteral("client.pem"));
        o.keyFile = certs.filePath(QStringLiteral("client-key.pem"));
        CHECK(c.connect(o) && c.capabilities(&caps));
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QProcess probe;
    probe.start(QStringLiteral(YANGSCOPE_PYTHON),
                {QStringLiteral("-c"), QStringLiteral("import grpc, grpc_tools, cryptography")});
    if (QStringLiteral(YANGSCOPE_PYTHON).isEmpty() || !probe.waitForFinished(20000) || probe.exitCode() != 0) {
        std::printf("gnmi: skipped (python3 with grpcio, grpcio-tools and cryptography not found)\n");
        return 77;
    }
    const QString data = QStringLiteral(YANGSCOPE_TEST_DATA);
    YangContext ctx;
    ctx.addSearchDir(data, false);
    ctx.addSearchDir(QStringLiteral(YANGSCOPE_TEST_NETCONF "/ietf"), false);
    ctx.loadFile(data + "/ys-test.yang");
    ctx.loadFile(data + "/ys-test-aug.yang");
    const QStringList ifFeat{QStringLiteral("if-mib")};
    ctx.loadModule("ietf-interfaces", "2018-02-20", &ifFeat);
    ctx.loadModule("ietf-ip", "2018-02-22");
    ctx.loadModule("iana-if-type", "2014-05-08");

    testPlain(ctx);
    testTls();
    std::printf("gnmi: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
