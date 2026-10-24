// SPDX-License-Identifier: GPL-3.0-or-later
// YangScope - native YANG / NETCONF / gNMI explorer
#include "gui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("YangScope"));
    QApplication::setOrganizationName(QStringLiteral("YangScope"));
    QApplication::setApplicationVersion(QStringLiteral(YANGSCOPE_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native YANG schema explorer"));
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
    if (!paths.isEmpty() || !selectPath.isEmpty() || !findText.isEmpty()) {
        QTimer::singleShot(0, &w, [&w, paths, rec, selectPath, findText] {
            if (!paths.isEmpty())
                w.openPaths(paths, rec);
            if (!findText.isEmpty())
                w.find(findText);
            if (!selectPath.isEmpty())
                w.selectPath(selectPath);
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
