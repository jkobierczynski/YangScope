// SPDX-License-Identifier: GPL-3.0-or-later
// gNMI paths, gNMI data -> libyang, and Set validation (no device needed).
#include "core/DataTree.h"
#include "core/GnmiData.h"
#include "core/GnmiPath.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QCoreApplication>

#include <cstdio>

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

static Path P(const QString &s)
{
    Path p;
    QString err;
    if (!parsePath(s, &p, &err))
        std::fprintf(stderr, "parse failed: %s: %s\n", qPrintable(s), qPrintable(err));
    return p;
}

static Value json(const QByteArray &j)
{
    Value v;
    v.kind = Value::JsonIetf;
    v.json = j;
    return v;
}

static Value scalar(Value::Kind k, const QString &t)
{
    Value v;
    v.kind = k;
    v.text = t;
    return v;
}

static void dump(const EditValidation &v)
{
    for (const QString &e : v.errors)
        std::fprintf(stderr, "    error: %s\n", qPrintable(e));
    for (const QString &w : v.warnings)
        std::fprintf(stderr, "    warning: %s\n", qPrintable(w));
    for (const EditChange &c : v.changes)
        std::fprintf(stderr, "    %s\n", qPrintable(c.toString()));
}

static bool hasChange(const EditValidation &v, const QString &t)
{
    for (const EditChange &c : v.changes)
        if (c.toString() == t)
            return true;
    return false;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- paths ---------------------------------------------------------------
    Path p = P("/interfaces/interface[name=eth0]/state/counters");
    CHECK(p.elems.size() == 4);
    CHECK(p.elems[1].keys.size() == 1 && p.elems[1].keys[0].second == QLatin1String("eth0"));
    CHECK_EQ(pathToString(p), "/interfaces/interface[name=eth0]/state/counters");
    p = P("openconfig:/network-instances/network-instance[name=default]/protocols/protocol[identifier=BGP][name=bgp]");
    CHECK_EQ(p.origin, "openconfig");
    CHECK(p.elems[3].keys.size() == 2);
    p = P("/a/b[k=x\\]y/z]/c");
    CHECK_EQ(p.elems[1].keys[0].second, "x]y/z");
    CHECK_EQ(pathToString(p), "/a/b[k=x\\]y/z]/c");
    p = P("/acl/entry[prefix=10.0.0.0/8]");
    CHECK_EQ(p.elems[1].keys[0].second, "10.0.0.0/8");
    CHECK(P("/").isRoot());
    {
        Path bad;
        QString err;
        CHECK(!parsePath("/a/b[k=1", &bad, &err) && err.contains("]"));
        CHECK(!parsePath("/a//b", &bad, &err));
        CHECK(!parsePath("/a/b[=1]", &bad, &err));
    }

    // --- schemas --------------------------------------------------------------
    const QString data = QStringLiteral(YANGSCOPE_TEST_DATA);
    YangContext ctx;
    ctx.addSearchDir(data, false);
    ctx.addSearchDir(QStringLiteral(YANGSCOPE_TEST_NETCONF "/ietf"), false);
    CHECK(ctx.loadFile(data + "/ys-test.yang").ok);
    CHECK(ctx.loadFile(data + "/ys-test-aug.yang").ok);
    const QStringList ifFeat{QStringLiteral("if-mib")};
    CHECK(ctx.loadModule("ietf-interfaces", "2018-02-20", &ifFeat).ok);
    CHECK(ctx.loadModule("ietf-ip", "2018-02-22").ok);
    CHECK(ctx.loadModule("iana-if-type", "2014-05-08").ok);

    // schema node -> gNMI path
    const lysc_node *srvSchema = lys_find_path(ctx.raw(), nullptr, "/ys-test:system/server", 0);
    CHECK_EQ(pathToString(pathForSchemaNode(srvSchema, false)), "/system/server[name=*][port=*]");
    CHECK_EQ(pathToString(pathForSchemaNode(srvSchema, true)), "/ys-test:system/server[name=*][port=*]");

    // resolve: with/without module prefixes, augments, keys
    Resolved r = resolve(ctx.raw(), P("/system/server[name=ntp1][port=123]/comment"));
    CHECK(r.ok());
    CHECK_EQ(r.lyPath, "/ys-test:system/server[name='ntp1'][port='123']/ys-test-aug:comment");
    r = resolve(ctx.raw(), P("/ietf-interfaces:interfaces/interface[name=*]/ietf-ip:ipv4/mtu"));
    CHECK(r.ok() && r.lyPath.isEmpty());
    r = resolve(ctx.raw(), P("/system/server[colour=red]"));
    CHECK(!r.ok() && r.error.contains("not a key"));
    r = resolve(ctx.raw(), P("/system/nothing"));
    CHECK(!r.ok());
    r = resolve(ctx.raw(), P("/system/server[name=a][port=1]/v4"));  // through choice/case
    CHECK(r.ok() && r.lyPath.endsWith("/v4"));

    // --- notifications -> tree -------------------------------------------------
    QVector<Notification> notes(1);
    notes[0].prefix = P("/interfaces/interface[name=eth0]");
    Update u1;
    u1.path = P("/interfaces/interface[name=eth0]/statistics/in-octets");
    u1.value = scalar(Value::Uint, "918273645");
    Update u2;
    u2.path = P("/interfaces/interface[name=eth0]/if-index");
    u2.value = scalar(Value::Int, "2");
    Update u3;
    u3.path = P("/interfaces/interface[name=eth0]");
    u3.value = json(R"({"description":"uplink","type":"iana-if-type:ethernetCsmacd","enabled":true,
                       "ietf-ip:ipv4":{"mtu":1500}})");
    Update u4;   // whole list without keys: array value
    u4.path = P("/system/server");
    u4.value = json(R"([{"name":"ntp1","port":123,"weight":"60.00","ys-test-aug:comment":"x"},
                        {"name":"ntp2","port":123,"v6":"2001:db8::10"}])");
    Update u5;   // leaf with a JSON scalar
    u5.path = P("/ys-test:system/hostname");
    u5.value = json(R"("lab-router-1")");
    Update u6;   // server that wraps the content in its own name
    u6.path = P("/system/status");
    u6.value = json(R"({"ys-test:status":{"uptime":"86400","alarms":["fan","psu"]}})");
    notes[0].updates = {u1, u2, u3, u4, u5, u6};
    notes[0].deletes = {P("/interfaces/interface[name=eth9]")};
    DataTree tree;
    QStringList msgs;
    const int placed = notificationsToTree(ctx.raw(), notes, &tree, &msgs);
    CHECK(placed == 6);
    for (const QString &m : msgs)
        if (!m.startsWith("deleted"))
            std::fprintf(stderr, "  msg: %s\n", qPrintable(m));
    CHECK(msgs.contains("deleted: /interfaces/interface[name=eth9]"));
    const QString js = tree.toJson();
    CHECK(js.contains("\"in-octets\": \"918273645\""));
    CHECK(js.contains("\"if-index\": 2"));
    CHECK(js.contains("\"ietf-ip:ipv4\""));
    CHECK(js.contains("\"ys-test-aug:comment\": \"x\""));
    CHECK(js.contains("\"uptime\": \"86400\""));
    CHECK(js.contains("\"hostname\": \"lab-router-1\""));
    if (placed != 6)
        std::fprintf(stderr, "%s\n", qPrintable(js));

    // data node -> path and JSON
    const lyd_node *sys = nullptr;
    for (const lyd_node *n = tree.root(); n; n = DataTree::next(n))
        if (DataTree::name(n) == QLatin1String("ys-test:system"))
            sys = n;
    CHECK(sys);
    const lyd_node *ntp1 = DataTree::firstChild(sys);
    while (ntp1 && !DataTree::name(ntp1).startsWith("server"))
        ntp1 = DataTree::next(ntp1);
    CHECK(ntp1);
    if (ntp1) {
        CHECK_EQ(pathToString(pathForDataNode(ntp1, false)), "/system/server[name=ntp1][port=123]");
        const QByteArray ej = entryJson(ntp1);
        CHECK(ej.contains("\"weight\": \"60.0") && ej.contains("ys-test-aug:comment"));
    }

    // --- Set script --------------------------------------------------------------
    QVector<SetOperation> ops;
    QStringList errs;
    CHECK(parseSetScript(
        "# change two things\n"
        "update /system/hostname = r2\n"
        "replace /system/server[name=ntp1][port=123] = {\n"
        "  \"weight\": \"75.00\"\n"
        "}\n"
        "delete /system/server[name=ntp2][port=123]\n", &ops, &errs));
    CHECK(ops.size() == 3);
    CHECK(ops.value(0).json == "\"r2\"");
    CHECK(ops.value(1).kind == SetOperation::Replace && ops.value(1).line == 3);
    CHECK(ops.value(2).kind == SetOperation::Delete);
    CHECK(formatSetOperation(ops.value(0)) == QLatin1String("update /system/hostname = \"r2\""));
    errs.clear();
    CHECK(!parseSetScript("update /system/hostname\n", &ops, &errs) && !errs.isEmpty());
    errs.clear();
    CHECK(!parseSetScript("frobnicate /x = 1\n", &ops, &errs));
    errs.clear();
    CHECK(!parseSetScript("update /x = {\"a\": \n", &ops, &errs));

    // current config for validation
    DataTree cfg;
    CHECK(cfg.parseJson(ctx.raw(), R"({"ys-test:system":{"hostname":"lab-router-1","mode":"cluster",
        "server":[{"name":"ntp1","port":123,"proto":"udp","weight":"60.00","v4":"192.0.2.10",
                   "ys-test-aug:comment":"primary"},
                  {"name":"ntp2","port":123,"proto":"udp","weight":"40.00","v6":"2001:db8::10"}]}})"));

    auto validate = [&](const QString &script) {
        QVector<SetOperation> o;
        QStringList e;
        if (!parseSetScript(script, &o, &e)) {
            EditValidation v;
            v.errors = e;
            return v;
        }
        return validateSet(ctx.raw(), o, &cfg);
    };

    EditValidation v = validate("update /system/hostname = r2");
    CHECK(v.ok && hasChange(v, "~ /ys-test:system/hostname: lab-router-1 -> r2"));
    if (!v.ok) dump(v);

    v = validate("replace /system/server[name=ntp1][port=123] = {\"weight\": \"75.00\"}");
    CHECK(v.ok);
    CHECK(hasChange(v, "- /ys-test:system/server[name='ntp1'][port='123']/v4"));
    CHECK(hasChange(v, "~ /ys-test:system/server[name='ntp1'][port='123']/weight: 60.0 -> 75.0"));
    if (!v.ok || v.changes.size() < 3) dump(v);

    v = validate("update /system/server[name=ntp1][port=123]/weight = heavy");
    CHECK(!v.ok && !v.schemaValid);

    v = validate("delete /system/hostname");
    CHECK(v.schemaValid && !v.ok);  // mandatory

    v = validate("update /system/preferred-server = ghost");
    CHECK(v.schemaValid && !v.ok);  // leafref
    v = validate("update /system/preferred-server = ntp2\ndelete /system/server[name=ntp2][port=123]");
    CHECK(!v.ok);                    // deletes run first, then the leafref breaks
    v = validate("update /system/preferred-server = ntp1\ndelete /system/server[name=ntp2][port=123]");
    CHECK(v.ok && hasChange(v, "- /ys-test:system/server[name='ntp2'][port='123']"));

    v = validate("update /system/status/uptime = 5");
    CHECK(!v.ok && v.errors.value(0).contains("state data"));
    v = validate("update /system/server[name=*][port=*]/weight = 1.00");
    CHECK(!v.ok && v.errors.value(0).contains("wildcards"));
    v = validate("delete /system/server[name=nope][port=1]");
    CHECK(v.ok && !v.warnings.isEmpty());

    // new list entry with an identityref and an augment
    v = validate("update /system/server[name=ntp3][port=123] = {\"proto\": \"ys-test:tcp\", "
                 "\"ys-test-aug:comment\": \"lab\"}");
    CHECK(v.ok && hasChange(v, "+ /ys-test:system/server[name='ntp3'][port='123']/ys-test-aug:comment = lab"));
    if (!v.ok) dump(v);

    // round trip: the JSON of an entry as a replace changes nothing
    {
        const lyd_node *cs = cfg.root();
        const lyd_node *e = DataTree::firstChild(cs);
        while (e && !DataTree::name(e).startsWith("server"))
            e = DataTree::next(e);
        SetOperation op;
        op.kind = SetOperation::Replace;
        op.path = pathForDataNode(e, false);
        op.json = entryJson(e);
        op.line = 1;
        const EditValidation rt = validateSet(ctx.raw(), {op}, &cfg);
        CHECK(rt.ok && rt.changes.isEmpty());
        if (!rt.ok || !rt.changes.isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(formatSetOperation(op)));
            dump(rt);
        }
    }

    // schema-only validation
    {
        QVector<SetOperation> o;
        QStringList e;
        parseSetScript("delete /system/hostname", &o, &e);
        const EditValidation so = validateSet(ctx.raw(), o, nullptr);
        CHECK(so.ok && !so.fullValidation);
    }

    std::printf("gnmi-data: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
