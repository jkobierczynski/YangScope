// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained tests for the non-GUI core.
#include "core/NodeInfo.h"
#include "core/YangContext.h"

#include <libyang/libyang.h>

#include <QCoreApplication>
#include <cstdio>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        const QString a_ = (actual);                                                 \
        const QString e_ = (expected);                                               \
        if (a_ != e_) {                                                              \
            std::fprintf(stderr, "%s:%d: FAIL %s\n  got:      %s\n  expected: %s\n", \
                         __FILE__, __LINE__, #actual, qPrintable(a_), qPrintable(e_)); \
            ++g_failed;                                                              \
        } else {                                                                     \
            ++g_passed;                                                              \
        }                                                                            \
    } while (0)

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failed;                                                              \
        } else {                                                                     \
            ++g_passed;                                                              \
        }                                                                            \
    } while (0)

static const lysc_node *find(ys::YangContext &ctx, const char *path, bool output = false)
{
    const lysc_node *n = lys_find_path(ctx.raw(), nullptr, path, output);
    if (!n)
        std::fprintf(stderr, "node not found: %s\n", path);
    return n;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString data = QStringLiteral(YANGSCOPE_TEST_DATA);

    ys::YangContext ctx;
    ctx.addSearchDir(data, false);
    auto r1 = ctx.loadFile(data + QStringLiteral("/ys-test.yang"));
    for (const QString &m : r1.messages)
        std::fprintf(stderr, "  %s\n", qPrintable(m));
    CHECK(r1.ok);
    auto r2 = ctx.loadFile(data + QStringLiteral("/ys-test-aug.yang"));
    for (const QString &m : r2.messages)
        std::fprintf(stderr, "  %s\n", qPrintable(m));
    CHECK(r2.ok);
    if (!r1.ok || !r2.ok)
        return 1;

    // Top level: containers system and extras (from the submodule),
    // rpc reboot, notification booted
    const auto top = ys::topLevelOf(r1.module);
    CHECK(top.size() == 4);
    CHECK(find(ctx, "/ys-test:extras/note"));

    // Submodules are recognised and skipped when loaded directly
    CHECK(ys::YangContext::isSubmoduleFile(data + QStringLiteral("/ys-test-sub.yang")));
    CHECK(!ys::YangContext::isSubmoduleFile(data + QStringLiteral("/ys-test.yang")));
    CHECK(ctx.loadFile(data + QStringLiteral("/ys-test-sub.yang")).skipped);

    // Loading a folder: every non-submodule file loads
    {
        ys::YangContext folderCtx;
        int ok = 0, skipped = 0;
        for (const QString &f : ys::YangContext::yangFilesIn(data, false)) {
            const auto r = folderCtx.loadFile(f);
            ok += r.ok;
            skipped += r.skipped;
        }
        CHECK(ok == 2);
        CHECK(skipped == 1);
    }

    const lysc_node *server = find(ctx, "/ys-test:system/server");
    CHECK(server);
    CHECK_EQ(ys::nodeKind(server), "list");
    CHECK_EQ(ys::listKeys(server).join(','), "name,port");
    CHECK_EQ(ys::accessShort(ys::access(server)), "rw");
    CHECK_EQ(ys::flagsShort(server), "*");
    CHECK_EQ(ys::dataPath(server), "/ys-test:system/server");
    CHECK_EQ(ys::gnmiPath(server, false), "/system/server[name=*][port=*]");
    CHECK_EQ(ys::restconfPath(server), "/restconf/data/ys-test:system/server={name},{port}");

    // children: data nodes, choice, augmented leaf and the action
    QStringList kids;
    for (const lysc_node *c : ys::childrenOf(server))
        kids << QString::fromUtf8(c->name);
    CHECK(kids.contains("address"));
    CHECK(kids.contains("comment"));
    CHECK(kids.contains("restart"));

    const lysc_node *port = find(ctx, "/ys-test:system/server/port");
    CHECK_EQ(ys::typeName(port), "port-number");
    CHECK_EQ(ys::baseTypeName(port), "uint16");
    CHECK(ys::isMandatory(port)); // key
    CHECK(ys::nodeDetailsHtml(port).contains("1..65535"));

    const lysc_node *weight = find(ctx, "/ys-test:system/server/weight");
    CHECK(ys::nodeDetailsHtml(weight).contains("0.00..100.00"));
    CHECK(ys::nodeDetailsHtml(weight).contains("percent"));

    const lysc_node *proto = find(ctx, "/ys-test:system/server/proto");
    CHECK(ys::nodeDetailsHtml(proto).contains("ys-test:tcp"));

    // choice/case are in the schema path but not in the data path
    const lysc_node *v4 = find(ctx, "/ys-test:system/server/v4");
    CHECK(v4);
    CHECK(ys::schemaPath(v4).contains("address"));
    CHECK_EQ(ys::dataPath(v4), "/ys-test:system/server/v4");

    // augmented node: namespace change appears in rfc7951-style paths
    const lysc_node *comment = find(ctx, "/ys-test:system/server/ys-test-aug:comment");
    CHECK(comment);
    CHECK_EQ(ys::gnmiPath(comment, true),
             "/ys-test:system/server[name=*][port=*]/ys-test-aug:comment");
    CHECK(ys::subtreeFilter(comment).contains("xmlns=\"urn:yangscope:test-aug\""));

    const lysc_node *hostname = find(ctx, "/ys-test:system/hostname");
    CHECK_EQ(ys::flagsShort(hostname), "");
    CHECK(ys::nodeDetailsHtml(hostname).contains("1..63"));
    CHECK_EQ(ys::subtreeFilter(hostname),
             "<filter type=\"subtree\">\n"
             "  <system xmlns=\"urn:yangscope:test\">\n"
             "    <hostname/>\n"
             "  </system>\n"
             "</filter>");

    const lysc_node *mode = find(ctx, "/ys-test:system/mode");
    CHECK_EQ(ys::flagsShort(mode), "?");
    CHECK(ys::nodeDetailsHtml(mode).contains("cluster"));

    const lysc_node *uptime = find(ctx, "/ys-test:system/status/uptime");
    CHECK_EQ(ys::accessShort(ys::access(uptime)), "ro");

    const lysc_node *fancy = find(ctx, "/ys-test:system/fancy-settings");
    CHECK(fancy); // feature enabled via "*"
    CHECK_EQ(ys::flagsShort(fancy), "!");

    // RPC input / output access
    const lysc_node *delayIn = find(ctx, "/ys-test:reboot/delay");
    CHECK_EQ(ys::accessShort(ys::access(delayIn)), "-w");
    const lysc_node *sched = find(ctx, "/ys-test:reboot/scheduled-at", true);
    CHECK_EQ(ys::accessShort(ys::access(sched)), "ro");
    CHECK_EQ(ys::gnmiPath(sched, false), "");
    CHECK_EQ(ys::restconfPath(delayIn), "/restconf/operations/ys-test:reboot/delay");

    // Tree printer and module source
    CHECK(ys::moduleTreeText(r1.module).contains("+--rw system"));
    CHECK(ys::moduleSource(r1.module).contains("module ys-test"));
    CHECK(ys::moduleDetailsHtml(r1.module).contains("urn:yangscope:test"));

    // Module listing
    bool sawAug = false;
    for (const lys_module *m : ctx.implementedModules())
        sawAug |= qstrcmp(m->name, "ys-test-aug") == 0;
    CHECK(sawAug);

    std::printf("libyang %s: %d passed, %d failed\n",
                qPrintable(ys::YangContext::libyangVersion()), g_passed, g_failed);
    return g_failed ? 1 : 0;
}
