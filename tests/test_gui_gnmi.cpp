// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end gNMI GUI test on the offscreen platform: connect, load the
// target's models, Get, Get one entry, edit it with a validated Set, and
// subscribe with a live table and graph.
#include "gui_helpers.h"

#include "core/DataTree.h"
#include "core/GnmiClient.h"
#include "gui/DataTreeWidget.h"
#include "gui/GnmiPanel.h"
#include "gui/LiveGraph.h"
#include "gui/MainWindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QApplication::setOrganizationName(QStringLiteral("YangScopeTest"));
    QApplication::setApplicationName(QStringLiteral("YangScopeTest"));

    QProcess probe;
    probe.start(QStringLiteral(YANGSCOPE_PYTHON), {QStringLiteral("-c"), QStringLiteral("import grpc, grpc_tools")});
    if (QStringLiteral(YANGSCOPE_PYTHON).isEmpty() || !probe.waitForFinished(20000) || probe.exitCode() != 0) {
        std::printf("gui-gnmi: skipped (python3 with grpcio and grpcio-tools not found)\n");
        return 77;
    }
    QProcess server;
    const int port = startServer(server, QStringLiteral(YANGSCOPE_PYTHON),
                                 {QStringLiteral(YANGSCOPE_TEST_GNMI "/test_server.py"), QStringLiteral("--port"),
                                  QStringLiteral("0"), QStringLiteral("--tick"), QStringLiteral("0.1"),
                                  QStringLiteral("--user"), QStringLiteral("admin"), QStringLiteral("--password"),
                                  QStringLiteral("admin")});
    CHECK(port > 0);
    if (port <= 0)
        return 1;
    qputenv("YANGSCOPE_GNMI_PASSWORD", "admin");
    const QString shots = qEnvironmentVariable("YANGSCOPE_TEST_SCREENSHOTS");

    ys::MainWindow w;
    w.resize(1400, 900);
    w.show();
    // models are found in the search directories
    w.addSearchDirs({QStringLiteral(YANGSCOPE_TEST_DATA), QStringLiteral(YANGSCOPE_TEST_NETCONF "/ietf")}, false);
    w.gnmiFromCommandLine(QStringLiteral("admin@127.0.0.1:%1").arg(port), true, QString(), QString(), true,
                          QStringLiteral("/ys-test:system/server"), true, false);

    auto *panel = w.findChild<ys::GnmiPanel *>();
    auto *dataTree = w.findChild<ys::DataTreeWidget *>(QStringLiteral("gnmiDataTree"));
    auto *getPath = w.findChild<QLineEdit *>(QStringLiteral("gnmiGetPath"));
    auto *setEditor = w.findChild<QPlainTextEdit *>(QStringLiteral("gnmiSetEditor"));
    auto *validateBtn = w.findChild<QPushButton *>(QStringLiteral("gnmiValidateBtn"));
    auto *setBtn = w.findChild<QPushButton *>(QStringLiteral("gnmiSetBtn"));
    auto *getBtn = w.findChild<QPushButton *>(QStringLiteral("gnmiGetBtn"));
    auto *validation = w.findChild<QTextBrowser *>(QStringLiteral("gnmiValidation"));
    auto *subPaths = w.findChild<QPlainTextEdit *>(QStringLiteral("gnmiSubPaths"));
    auto *subscribeBtn = w.findChild<QPushButton *>(QStringLiteral("gnmiSubscribeBtn"));
    auto *stopBtn = w.findChild<QPushButton *>(QStringLiteral("gnmiStopBtn"));
    auto *interval = w.findChild<QDoubleSpinBox *>(QStringLiteral("gnmiInterval"));
    auto *live = w.findChild<QTreeWidget *>(QStringLiteral("gnmiLive"));
    auto *opCombo = w.findChild<QComboBox *>(QStringLiteral("gnmiOp"));
    CHECK(panel && dataTree && getPath && setEditor && validateBtn && setBtn && getBtn && validation && subPaths
          && subscribeBtn && stopBtn && interval && live && opCombo);
    if (!panel || !dataTree || !getPath || !setEditor || !validateBtn || !setBtn || !getBtn || !validation
        || !subPaths || !subscribeBtn || !stopBtn || !interval || !live || !opCombo)
        return 1;
    auto nodeOf = [](QTreeWidgetItem *it) {
        return static_cast<const lyd_node *>(it->data(0, Qt::UserRole + 3).value<const void *>());
    };
    const QRegularExpression serverRow(QStringLiteral("^server \\[name=ntp\\d"));
    const QRegularExpression ntp1Row(QStringLiteral("^server \\[name=ntp1"));

    // 1. connected, models loaded, Get /system/server placed in the schema tree
    CHECK(waitFor([&] { return countItems(dataTree, serverRow) == 2; }, 40000));
    CHECK(panel->capabilities().models.size() == 5);
    CHECK(getPath->text() == QLatin1String("/system/server[name=*][port=*]"));

    // 2. Get one entry from the data tree
    QTreeWidgetItem *ntp1 = findItem(dataTree, ntp1Row);
    CHECK(ntp1);
    if (!ntp1)
        return 1;
    emit dataTree->entryAction(nodeOf(ntp1), ys::DataTreeWidget::EntryAction::FilterSubtree);
    CHECK(getPath->text() == QLatin1String("/system/server[name=ntp1][port=123]"));
    CHECK(waitFor([&] { return getBtn->isEnabled() && countItems(dataTree, serverRow) == 1; }));

    // 3. edit the entry as a Set, validate locally
    ntp1 = findItem(dataTree, ntp1Row);
    CHECK(ntp1);
    if (!ntp1)
        return 1;
    emit dataTree->entryAction(nodeOf(ntp1), ys::DataTreeWidget::EntryAction::Edit);
    QString script = setEditor->toPlainText();
    CHECK(script.startsWith(QLatin1String("replace /system/server[name=ntp1][port=123] =")));
    CHECK(script.contains(QLatin1String("ys-test-aug:comment")));
    script.replace(QRegularExpression(QStringLiteral("\"weight\": \"[^\"]*\"")), QStringLiteral("\"weight\": \"75.00\""));
    setEditor->setPlainText(script);
    validateBtn->click();
    CHECK(waitFor([&] { return setBtn->isEnabled() && validation->toPlainText().contains(QLatin1String("Valid")); }));
    CHECK(validation->toPlainText().contains(QLatin1String("weight: 60.0 -> 75.0")));
    if (!shots.isEmpty())
        w.grab().save(shots + QStringLiteral("/gnmi-set.png"));

    // invalid value caught locally
    {
        QString bad = script;
        bad.replace(QStringLiteral("\"75.00\""), QStringLiteral("\"heavy\""));
        setEditor->setPlainText(bad);
        validateBtn->click();
        CHECK(waitFor([&] { return setBtn->isEnabled() && validation->toPlainText().contains(QLatin1String("Not valid")); }));
        setEditor->setPlainText(script);
    }

    // 4. Validate & Set through the confirmation dialog
    DialogAnswer setDlg{QStringLiteral("Set"), QString()};
    armAnswer(&setDlg);
    setBtn->click();
    CHECK(waitFor([&] { return validation->toPlainText().contains(QLatin1String("Set accepted")); }, 20000));
    CHECK(setDlg.answered && setDlg.seenText.contains(QLatin1String("weight")));
    {
        ys::gnmi::Client c;
        ys::gnmi::ConnectParams p;
        p.host = QStringLiteral("127.0.0.1");
        p.port = port;
        p.tls = ys::gnmi::TlsMode::Insecure;
        p.username = p.password = QStringLiteral("admin");
        CHECK(c.connect(p));
        QVector<ys::gnmi::Notification> notes;
        ys::gnmi::Path path;
        ys::gnmi::parsePath(QStringLiteral("/system/server[name=ntp1][port=123]/weight"), &path);
        CHECK(c.get({}, {path}, ys::gnmi::DataType::Config, ys::gnmi::Encoding::JsonIetf, &notes));
        CHECK(!notes.isEmpty() && !notes[0].updates.isEmpty()
              && notes[0].updates[0].value.json.contains("75"));
    }

    // 5. subscribe to eth0 counters, SAMPLE every 0.3 s
    opCombo->setCurrentIndex(opCombo->findText(QStringLiteral("Subscribe")));
    subPaths->setPlainText(QStringLiteral("/interfaces/interface[name=eth0]/statistics\n/interfaces/interface[name=eth0]/oper-status"));
    interval->setValue(0.3);
    subscribeBtn->click();
    const QRegularExpression inOctets(QStringLiteral("in-octets$"));
    CHECK(waitFor([&] {
        QTreeWidgetItem *it = findItem(live, inOctets);
        return it && it->text(3).toInt() >= (shots.isEmpty() ? 8 : 40) && !it->text(2).isEmpty();
    }, 40000));
    QTreeWidgetItem *row = findItem(live, inOctets);
    CHECK(row && row->text(2).toDouble() >= 0); // rate shown
    CHECK(findItem(live, QRegularExpression(QStringLiteral("oper-status$"))));
    if (!shots.isEmpty())
        w.grab().save(shots + QStringLiteral("/gnmi-live.png"));
    stopBtn->click();
    CHECK(waitFor([&] { return subscribeBtn->isEnabled(); }));

    server.kill();
    server.waitForFinished(2000);
    std::printf("gui-gnmi: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
