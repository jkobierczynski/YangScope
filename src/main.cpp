// SPDX-License-Identifier: GPL-3.0-or-later
// YangScope - native YANG / NETCONF / gNMI explorer
#include "gui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("YangScope"));
    QApplication::setOrganizationName(QStringLiteral("YangScope"));
    QApplication::setApplicationVersion(QStringLiteral(YANGSCOPE_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/yangscope.png")));
    QApplication::setDesktopFileName(QStringLiteral("yangscope"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native YANG schema explorer with NETCONF and gNMI clients"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption searchDir({QStringLiteral("p"), QStringLiteral("path")},
                                 QStringLiteral("Add a search directory for imports (repeatable)."),
                                 QStringLiteral("dir"));
    QCommandLineOption recursive({QStringLiteral("r"), QStringLiteral("recursive")},
                                 QStringLiteral("Load folders and search directories recursively."));
    QCommandLineOption select({QStringLiteral("s"), QStringLiteral("select")},
                              QStringLiteral("Select a node by path after loading, "
                                             "e.g. /ietf-interfaces:interfaces/interface."),
                              QStringLiteral("path"));
    QCommandLineOption findOpt({QStringLiteral("f"), QStringLiteral("find")},
                               QStringLiteral("Search for nodes after loading."),
                               QStringLiteral("text"));
    parser.addOption(findOpt);
    QCommandLineOption connectOpt({QStringLiteral("c"), QStringLiteral("connect")},
                                  QStringLiteral("Connect to a NETCONF device: [user@]host[:port]."),
                                  QStringLiteral("target"));
    QCommandLineOption deviceSchemas(QStringLiteral("device-schemas"),
                                     QStringLiteral("After connecting, download and load the device's schemas."));
    QCommandLineOption getOpt(QStringLiteral("get"),
                              QStringLiteral("After connecting, send a NETCONF get for the --select node."));
    QCommandLineOption gnmiOpt(QStringLiteral("gnmi"),
                               QStringLiteral("Connect to a gNMI target: [user@]host[:port] (default port 57400)."),
                               QStringLiteral("target"));
    QCommandLineOption gnmiInsecure(QStringLiteral("gnmi-insecure"),
                                    QStringLiteral("gNMI without TLS (labs only)."));
    QCommandLineOption gnmiCa(QStringLiteral("gnmi-ca"), QStringLiteral("CA certificate (PEM) for gNMI TLS."),
                              QStringLiteral("file"));
    QCommandLineOption gnmiName(QStringLiteral("gnmi-server-name"),
                                QStringLiteral("Name to verify in the target's TLS certificate."),
                                QStringLiteral("name"));
    QCommandLineOption gnmiSchemas(QStringLiteral("gnmi-schemas"),
                                   QStringLiteral("After connecting, load the target's models from the search directories."));
    QCommandLineOption subscribeOpt(QStringLiteral("subscribe"),
                                    QStringLiteral("After connecting (gNMI), subscribe to the --select node."));
    parser.addOption(gnmiOpt);
    parser.addOption(gnmiInsecure);
    parser.addOption(gnmiCa);
    parser.addOption(gnmiName);
    parser.addOption(gnmiSchemas);
    parser.addOption(subscribeOpt);
    parser.addOption(connectOpt);
    parser.addOption(deviceSchemas);
    parser.addOption(getOpt);
    parser.addOption(searchDir);
    parser.addOption(recursive);
    parser.addOption(select);
    parser.addPositionalArgument(QStringLiteral("paths"),
                                 QStringLiteral("YANG files or folders to open."),
                                 QStringLiteral("[paths...]"));
    parser.process(app);

    ys::MainWindow w;
    w.show();

    const bool rec = parser.isSet(recursive);
    if (parser.isSet(searchDir))
        w.addSearchDirs(parser.values(searchDir), rec);
    const QStringList paths = parser.positionalArguments();
    const QString selectPath = parser.value(select);
    const QString findText = parser.value(findOpt);
    const QString target = parser.value(connectOpt);
    const QString gnmiTarget = parser.value(gnmiOpt);
    const bool gInsecure = parser.isSet(gnmiInsecure);
    const QString gCa = parser.value(gnmiCa);
    const QString gName = parser.value(gnmiName);
    const bool gSchemas = parser.isSet(gnmiSchemas);
    const bool doSubscribe = parser.isSet(subscribeOpt);
    const bool loadDevice = parser.isSet(deviceSchemas);
    const bool doGet = parser.isSet(getOpt);
    if (!paths.isEmpty() || !selectPath.isEmpty() || !findText.isEmpty() || !target.isEmpty()
        || !gnmiTarget.isEmpty()) {
        QTimer::singleShot(0, &w, [&w, paths, rec, selectPath, findText, target, loadDevice, doGet,
                                   gnmiTarget, gInsecure, gCa, gName, gSchemas, doSubscribe] {
            if (!paths.isEmpty())
                w.openPaths(paths, rec);
            if (!findText.isEmpty())
                w.find(findText);
            if (!gnmiTarget.isEmpty())
                w.gnmiFromCommandLine(gnmiTarget, gInsecure, gCa, gName, gSchemas, selectPath,
                                      doGet && target.isEmpty(), doSubscribe);
            if (!target.isEmpty()) {
                // selection happens once the device (and its schemas) are there
                w.connectFromCommandLine(target, loadDevice, selectPath, doGet);
            } else if (!selectPath.isEmpty() && gnmiTarget.isEmpty()) {
                w.selectPath(selectPath);
            }
        });
    }
    // For documentation and CI smoke tests: save a screenshot and exit.
    const QString shot = qEnvironmentVariable("YANGSCOPE_SCREENSHOT");
    if (!shot.isEmpty()) {
        QTimer::singleShot(qEnvironmentVariableIntValue("YANGSCOPE_SCREENSHOT_DELAY_MS") > 0
                               ? qEnvironmentVariableIntValue("YANGSCOPE_SCREENSHOT_DELAY_MS")
                               : 1500,
                           &w, [&w, shot] {
                               w.grab().save(shot);
                               QApplication::quit();
                           });
    }
    return QApplication::exec();
}
