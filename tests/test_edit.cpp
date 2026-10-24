// SPDX-License-Identifier: GPL-3.0-or-later
// edit-config validation and XML built from data entries (no device needed).
#include "core/DataTree.h"
#include "core/EditConfig.h"
#include "core/NetconfXml.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QCoreApplication>
#include <QFile>

#include <cstdio>

using namespace ys;

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

static void dump(const char *what, const EditValidation &v)
{
    std::fprintf(stderr, "  [%s] ok=%d schema=%d full=%d\n", what, v.ok, v.schemaValid, v.fullValidation);
    for (const QString &e : v.errors)
        std::fprintf(stderr, "    error: %s\n", qPrintable(e));
    for (const QString &w : v.warnings)
        std::fprintf(stderr, "    warning: %s\n", qPrintable(w));
    for (const EditChange &c : v.changes)
        std::fprintf(stderr, "    %s\n", qPrintable(c.toString()));
}

static bool hasChange(const EditValidation &v, const QString &text)
{
    for (const EditChange &c : v.changes)
        if (c.toString() == text)
            return true;
    return false;
}

static bool anyError(const EditValidation &v, const QString &needle)
{
    for (const QString &e : v.errors)
        if (e.contains(needle, Qt::CaseInsensitive))
            return true;
    return false;
}

static const char *NS = "xmlns=\"urn:yangscope:test\"";

static QByteArray sys(const QString &inner)
{
    return QStringLiteral("<system %1>%2</system>").arg(QLatin1String(NS), inner).toUtf8();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString data = QStringLiteral(YANGSCOPE_TEST_DATA);
    YangContext ctx;
    ctx.addSearchDir(data, false);
    CHECK(ctx.loadFile(data + "/ys-test.yang").ok);
    CHECK(ctx.loadFile(data + "/ys-test-aug.yang").ok);

    // current configuration = tests/netconf/config.xml without the wrapper
    QFile f(QStringLiteral(YANGSCOPE_TEST_NETCONF "/config.xml"));
    CHECK(f.open(QIODevice::ReadOnly));
    QByteArray cfg = f.readAll();
    cfg = cfg.mid(cfg.indexOf("<system"));
    cfg = cfg.left(cfg.indexOf("<interfaces")); // ietf-interfaces is not loaded here
    DataTree current;
    CHECK(current.parseXml(ctx.raw(), cfg));
    CHECK(!current.isEmpty());

    // 1. plain value change, with a diff line
    auto v = validateEdit(ctx.raw(), sys("<hostname>r2</hostname>"), &current);
    CHECK(v.ok && v.fullValidation);
    CHECK(hasChange(v, "~ /ys-test:system/hostname: lab-router-1 -> r2"));
    if (!v.ok) dump("hostname", v);

    // 2. wrong type
    v = validateEdit(ctx.raw(), sys("<server><name>x</name><port>abc</port></server>"), &current);
    CHECK(!v.ok && !v.schemaValid);
    CHECK(anyError(v, "abc"));

    // 3. unknown element and missing namespace
    v = validateEdit(ctx.raw(), sys("<colour>blue</colour>"), &current);
    CHECK(!v.ok && anyError(v, "unknown element <colour>"));
    v = validateEdit(ctx.raw(), "<system><hostname>x</hostname></system>", &current);
    CHECK(!v.ok && anyError(v, "unknown element <system>"));

    // 4. state data cannot be edited
    v = validateEdit(ctx.raw(), sys("<status><uptime>1</uptime></status>"), &current);
    CHECK(!v.ok && anyError(v, "state data"));

    // 5. deleting a mandatory leaf fails full validation
    v = validateEdit(ctx.raw(), sys("<hostname nc:operation=\"delete\"/>"), &current);
    CHECK(v.schemaValid && !v.ok);
    CHECK(anyError(v, "mandatory"));
    if (v.ok) dump("delete mandatory", v);

    // 6./7. create on existing, delete on missing
    v = validateEdit(ctx.raw(), sys("<server nc:operation=\"create\"><name>ntp1</name><port>123</port></server>"), &current);
    CHECK(!v.ok && anyError(v, "data-exists"));
    v = validateEdit(ctx.raw(), sys("<server nc:operation=\"delete\"><name>nope</name><port>1</port></server>"), &current);
    CHECK(!v.ok && anyError(v, "data-missing"));
    // invalid operation value
    v = validateEdit(ctx.raw(), sys("<hostname nc:operation=\"zap\">x</hostname>"), &current);
    CHECK(!v.ok && anyError(v, "invalid operation"));

    // 8. delete an existing entry
    v = validateEdit(ctx.raw(), sys("<server nc:operation=\"delete\"><name>ntp2</name><port>123</port></server>"), &current);
    CHECK(v.ok);
    CHECK(hasChange(v, "- /ys-test:system/server[name='ntp2'][port='123']"));
    if (!v.ok) dump("delete ntp2", v);

    // 9. new entry with an identityref from the same module and an augment
    v = validateEdit(ctx.raw(),
                     sys("<server><name>ntp3</name><port>123</port><proto xmlns:yst=\"urn:yangscope:test\">yst:tcp</proto>"
                         "<comment xmlns=\"urn:yangscope:test-aug\">lab</comment></server>"),
                     &current);
    CHECK(v.ok);
    CHECK(hasChange(v, "+ /ys-test:system/server[name='ntp3'][port='123']/name = ntp3"));
    CHECK(hasChange(v, "+ /ys-test:system/server[name='ntp3'][port='123']/ys-test-aug:comment = lab"));
    if (!v.ok) dump("ntp3", v);

    // 10. replace drops what the new entry does not have
    v = validateEdit(ctx.raw(), sys("<server nc:operation=\"replace\"><name>ntp1</name><port>123</port><weight>10.00</weight></server>"), &current);
    CHECK(v.ok);
    CHECK(hasChange(v, "- /ys-test:system/server[name='ntp1'][port='123']/v4"));
    CHECK(hasChange(v, "- /ys-test:system/server[name='ntp1'][port='123']/ys-test-aug:comment"));
    if (!v.ok || v.changes.size() < 3) dump("replace", v);

    // 11. leafref must point at an existing server
    v = validateEdit(ctx.raw(), sys("<preferred-server>ghost</preferred-server>"), &current);
    CHECK(v.schemaValid && !v.ok);
    if (v.ok) dump("leafref", v);
    v = validateEdit(ctx.raw(), sys("<preferred-server>ntp2</preferred-server>"), &current);
    CHECK(v.ok);
    // ...and deleting the referenced server breaks it
    v = validateEdit(ctx.raw(), sys("<preferred-server>ntp2</preferred-server>"
                                    "<server nc:operation=\"delete\"><name>ntp2</name><port>123</port></server>"), &current);
    CHECK(!v.ok);

    // 12. default-operation none: only explicit operations apply
    v = validateEdit(ctx.raw(), sys("<mode>standalone</mode><hostname nc:operation=\"merge\">r9</hostname>"),
                     &current, DefaultOperation::None);
    CHECK(v.ok);
    CHECK(hasChange(v, "~ /ys-test:system/hostname: lab-router-1 -> r9"));
    CHECK(v.changes.size() == 1);
    if (v.changes.size() != 1) dump("none", v);

    // 13. without the current configuration: schema check only
    v = validateEdit(ctx.raw(), sys("<hostname nc:operation=\"delete\"/>"), nullptr);
    CHECK(v.ok && v.schemaValid && !v.fullValidation && !v.warnings.isEmpty());

    // 14. XML built from data entries round-trips through the validator
    const lyd_node *sysNode = current.root();
    const lyd_node *ntp1 = DataTree::firstChild(sysNode);
    while (ntp1 && QString::fromUtf8(DataTree::schema(ntp1)->name) != QLatin1String("server"))
        ntp1 = DataTree::next(ntp1);
    CHECK(ntp1);
    const QString cfgXml = entryEditXml(ntp1, EntryXml::Config);
    CHECK(cfgXml.contains("<name>ntp1</name>") || cfgXml.contains(">ntp1</name>"));
    CHECK(cfgXml.contains("urn:yangscope:test-aug"));
    v = validateEdit(ctx.raw(), cfgXml.toUtf8(), &current);
    CHECK(v.ok && v.changes.isEmpty());
    if (!v.ok) { std::fprintf(stderr, "%s\n", qPrintable(cfgXml)); dump("roundtrip", v); }

    const QString delXml = entryEditXml(ntp1, EntryXml::Delete);
    CHECK(delXml.contains("nc:operation=\"delete\""));
    v = validateEdit(ctx.raw(), delXml.toUtf8(), &current);
    CHECK(v.ok && hasChange(v, "- /ys-test:system/server[name='ntp1'][port='123']"));
    if (!v.ok) { std::fprintf(stderr, "%s\n", qPrintable(delXml)); dump("delete xml", v); }

    const QByteArray filt = entrySubtreeFilter(ntp1);
    CHECK(filt.startsWith("<filter type=\"subtree\"><system xmlns=\"urn:yangscope:test\"><server>"));
    CHECK(filt.contains(">ntp1</name>") && filt.contains(">123</port>"));
    CHECK(!filt.contains("weight"));
    QVector<QPair<QString, QString>> pfx;
    CHECK(entryXPath(ntp1, &pfx) == QLatin1String("/ys-test:system/server[name='ntp1'][port='123']"));
    CHECK(pfx.size() == 1 && pfx[0].first == QLatin1String("ys-test"));

    // state stripped when editing a subtree taken from <get> data
    DataTree withState;
    QFile d(QStringLiteral(YANGSCOPE_TEST_NETCONF "/data.xml"));
    CHECK(d.open(QIODevice::ReadOnly));
    QByteArray dx = d.readAll();
    dx = dx.mid(dx.indexOf("<system"));
    dx = dx.left(dx.indexOf("<interfaces"));
    CHECK(withState.parseXml(ctx.raw(), dx));
    const QString sysXml = entryEditXml(withState.root(), EntryXml::Config);
    CHECK(!sysXml.contains("uptime") && sysXml.contains("hostname"));

    // operation builders
    CHECK(netconf::editConfigOp("candidate", "<x/>", "none").contains("<default-operation>none</default-operation>"));
    CHECK(netconf::commitOp(120).contains("<confirm-timeout>120</confirm-timeout>"));
    CHECK(netconf::commitOp() == "<commit/>");

    std::printf("edit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
