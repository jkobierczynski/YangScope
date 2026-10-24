// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end GUI test: connect to the test server, load its schemas, filter
// on a list entry, edit it, validate locally, send, lock and commit.
// Runs on the offscreen platform; skipped (77) without python3 + paramiko.
#include "core/NetconfSession.h"
#include "gui/DataTreeWidget.h"
#include "gui/MainWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

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

static bool waitFor(const std::function<bool()> &cond, int ms = 15000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms)
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

/// Answers the next modal dialog by clicking the button with @p button text.
struct DialogAnswer {
    QString button;
    QString screenshot;
    bool answered = false;
    QString seenText;
};

static void armAnswer(DialogAnswer *a)
{
    auto *timer = new QTimer(qApp);
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, [a, timer] {
        QWidget *m = QApplication::activeModalWidget();
        if (!m)
            return;
        for (QLabel *l : m->findChildren<QLabel *>())
            a->seenText += l->text() + QLatin1Char('\n');
        for (QPlainTextEdit *e : m->findChildren<QPlainTextEdit *>())
            a->seenText += e->toPlainText() + QLatin1Char('\n');
        if (!a->screenshot.isEmpty())
            m->grab().save(a->screenshot);
        for (QAbstractButton *b : m->findChildren<QAbstractButton *>()) {
            if (b->text().remove(QLatin1Char('&')) == a->button) {
                a->answered = true;
                timer->stop();
                timer->deleteLater();
                b->click();
                return;
            }
        }
    });
    timer->start();
}

static QTreeWidgetItem *findItem(QTreeWidget *tree, const QRegularExpression &re)
{
    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) -> QTreeWidgetItem * {
        for (int i = 0; i < it->childCount(); ++i) {
            QTreeWidgetItem *c = it->child(i);
            if (re.match(c->text(0)).hasMatch())
                return c;
            if (QTreeWidgetItem *f = walk(c))
                return f;
        }
        return nullptr;
    };
    return walk(tree->invisibleRootItem());
}

static int countItems(QTreeWidget *tree, const QRegularExpression &re)
{
    int n = 0;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
        for (int i = 0; i < it->childCount(); ++i) {
            n += re.match(it->child(i)->text(0)).hasMatch();
            walk(it->child(i));
        }
    };
    walk(tree->invisibleRootItem());
    return n;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true); // settings and caches in a test location
    QApplication::setOrganizationName(QStringLiteral("YangScopeTest"));
    QApplication::setApplicationName(QStringLiteral("YangScopeTest"));

    QProcess probe;
    probe.start(QStringLiteral(YANGSCOPE_PYTHON), {QStringLiteral("-c"), QStringLiteral("import paramiko")});
    if (QStringLiteral(YANGSCOPE_PYTHON).isEmpty() || !probe.waitForFinished(10000) || probe.exitCode() != 0) {
        std::printf("gui: skipped (python3 with paramiko not found)\n");
        return 77;
    }

    QTemporaryDir tmp;
    const QString dir = QStringLiteral(YANGSCOPE_TEST_NETCONF);
    const QString hostKey = tmp.filePath(QStringLiteral("hostkey"));
    QStringList common{dir + QStringLiteral("/test_server.py"),
                       QStringLiteral("--schemas"), QStringLiteral(YANGSCOPE_TEST_DATA),
                       QStringLiteral("--schemas"), dir + QStringLiteral("/ietf"),
                       QStringLiteral("--feature"), QStringLiteral("ietf-interfaces=if-mib"),
                       QStringLiteral("--data"), dir + QStringLiteral("/data.xml"),
                       QStringLiteral("--config"), dir + QStringLiteral("/config.xml"),
                       QStringLiteral("--host-key"), hostKey};
    QProcess server;
    server.setProcessChannelMode(QProcess::MergedChannels);
    server.start(QStringLiteral(YANGSCOPE_PYTHON), common + QStringList{QStringLiteral("--port"), QStringLiteral("0")});
    int port = 0;
    while (server.waitForReadyRead(10000)) {
        const QByteArray out = server.readAll();
        const qsizetype i = out.indexOf("listening on ");
        if (i >= 0) {
            const QByteArray line = out.mid(i).split('\n').first().trimmed();
            port = line.mid(line.lastIndexOf(':') + 1).toInt();
            break;
        }
    }
    CHECK(port > 0);
    if (port <= 0)
        return 1;
    // known_hosts entry for the fixed host key
    QProcess kh;
    kh.start(QStringLiteral(YANGSCOPE_PYTHON), common + QStringList{QStringLiteral("--port"), QString::number(port),
                                                                     QStringLiteral("--known-hosts-line")});
    kh.waitForFinished(20000);
    const QString knownHosts = tmp.filePath(QStringLiteral("known_hosts"));
    {
        QFile f(knownHosts);
        f.open(QIODevice::WriteOnly);
        f.write(kh.readAllStandardOutput());
    }
    qputenv("YANGSCOPE_KNOWN_HOSTS", knownHosts.toUtf8());
    qputenv("YANGSCOPE_NETCONF_PASSWORD", "admin");
    const QString shots = qEnvironmentVariable("YANGSCOPE_TEST_SCREENSHOTS");

    ys::MainWindow w;
    w.resize(1400, 900);
    w.show();
    w.connectFromCommandLine(QStringLiteral("admin@127.0.0.1:%1").arg(port), true,
                             QStringLiteral("/ys-test:system/server"), true);

    auto *dataTree = w.findChild<ys::DataTreeWidget *>(QStringLiteral("dataTree"));
    auto *editor = w.findChild<QPlainTextEdit *>(QStringLiteral("requestEditor"));
    auto *validateBtn = w.findChild<QPushButton *>(QStringLiteral("validateBtn"));
    auto *sendBtn = w.findChild<QPushButton *>(QStringLiteral("sendBtn"));
    auto *lockBtn = w.findChild<QPushButton *>(QStringLiteral("lockBtn"));
    auto *unlockBtn = w.findChild<QPushButton *>(QStringLiteral("unlockBtn"));
    auto *commitBtn = w.findChild<QPushButton *>(QStringLiteral("commitBtn"));
    auto *validation = w.findChild<QTextBrowser *>(QStringLiteral("validationView"));
    auto *state = w.findChild<QLabel *>(QStringLiteral("candidateState"));
    CHECK(dataTree && editor && validateBtn && sendBtn && lockBtn && commitBtn && validation && state);
    if (!dataTree || !editor || !validateBtn || !sendBtn || !lockBtn || !commitBtn || !validation || !state)
        return 1;
    auto nodeOf = [](QTreeWidgetItem *it) {
        return static_cast<const lyd_node *>(it->data(0, Qt::UserRole + 3).value<const void *>());
    };
    const QRegularExpression serverRow(QStringLiteral("^server \\[name=ntp\\d"));
    const QRegularExpression ntp1Row(QStringLiteral("^server \\[name=ntp1"));

    // 1. connected, schemas loaded, get on /system/server shows both servers
    CHECK(waitFor([&] { return countItems(dataTree, serverRow) == 2; }, 30000));

    // 2. filter on one list entry
    QTreeWidgetItem *ntp1 = findItem(dataTree, ntp1Row);
    CHECK(ntp1);
    if (!ntp1)
        return 1;
    emit dataTree->entryAction(nodeOf(ntp1), ys::DataTreeWidget::EntryAction::FilterSubtree);
    CHECK(editor->toPlainText().contains(QLatin1String(">ntp1</name>")));
    CHECK(editor->toPlainText().contains(QLatin1String(">123</port>")));
    sendBtn->click();
    CHECK(waitFor([&] { return sendBtn->isEnabled() && countItems(dataTree, serverRow) == 1; }));

    // 3. lock the candidate
    lockBtn->click();
    CHECK(waitFor([&] { return state->text().contains(QLatin1String("locked by this session")); }));

    // 4. edit the entry, validate locally
    ntp1 = findItem(dataTree, ntp1Row);
    CHECK(ntp1);
    if (!ntp1)
        return 1;
    emit dataTree->entryAction(nodeOf(ntp1), ys::DataTreeWidget::EntryAction::Edit);
    QString xml = editor->toPlainText();
    CHECK(xml.contains(QLatin1String("<weight>")));
    CHECK(!xml.contains(QLatin1String("uptime")));
    xml.replace(QRegularExpression(QStringLiteral("<weight>[^<]*</weight>")), QStringLiteral("<weight>75.00</weight>"));
    editor->setPlainText(xml);
    validateBtn->click();
    CHECK(waitFor([&] { return sendBtn->isEnabled() && validation->toPlainText().contains(QLatin1String("Valid")); }));
    const QString report = validation->toPlainText();
    CHECK(report.contains(QLatin1String("Valid")) && !report.contains(QLatin1String("Not valid")));
    CHECK(report.contains(QLatin1String("weight")) && report.contains(QLatin1String("75")));
    if (!shots.isEmpty())
        w.grab().save(shots + QStringLiteral("/edit-validation.png"));

    // an invalid edit is caught before anything is sent
    {
        const QString good = editor->toPlainText();
        QString bad = good;
        bad.replace(QRegularExpression(QStringLiteral("<weight>[^<]*</weight>")), QStringLiteral("<weight>heavy</weight>"));
        editor->setPlainText(bad);
        validateBtn->click();
        CHECK(waitFor([&] { return sendBtn->isEnabled() && validation->toPlainText().contains(QLatin1String("Not valid")); }));
        editor->setPlainText(good);
    }

    // 5. validate & send: the confirmation lists the change
    DialogAnswer sendDlg{QStringLiteral("Send"), shots.isEmpty() ? QString() : shots + QStringLiteral("/edit-confirm.png")};
    armAnswer(&sendDlg);
    sendBtn->click();
    CHECK(waitFor([&] { return state->text().contains(QLatin1String("uncommitted")); }));
    CHECK(sendDlg.answered);
    CHECK(sendDlg.seenText.contains(QLatin1String("weight")));

    // 6. commit: the preview diffs candidate against running
    DialogAnswer commitDlg{QStringLiteral("Commit"), shots.isEmpty() ? QString() : shots + QStringLiteral("/commit-preview.png")};
    armAnswer(&commitDlg);
    commitBtn->click();
    CHECK(waitFor([&] { return commitDlg.answered && sendBtn->isEnabled()
                               && !state->text().contains(QLatin1String("uncommitted")); }, 20000));
    CHECK(commitDlg.seenText.contains(QLatin1String("1 changed")));

    // 7. running now has the new value (checked from a second session)
    {
        ys::netconf::Session s;
        ys::netconf::ConnectParams p;
        p.host = QStringLiteral("127.0.0.1");
        p.port = port;
        p.user = QStringLiteral("admin");
        p.password = QStringLiteral("admin");
        p.knownHostsFile = knownHosts;
        CHECK(s.open(p, {}));
        ys::netconf::Reply rep;
        CHECK(s.rpc(ys::netconf::getConfigOp(QStringLiteral("running")), &rep));
        CHECK(rep.data.contains("<weight>75.0") || rep.data.contains(">75.0"));
        s.close();
    }

    unlockBtn->click();
    CHECK(waitFor([&] { return state->text().contains(QLatin1String("not locked")); }));

    server.kill();
    server.waitForFinished(2000);
    std::printf("gui: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
