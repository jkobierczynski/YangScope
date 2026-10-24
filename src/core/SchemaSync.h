// SPDX-License-Identifier: GPL-3.0-or-later
// Find out which YANG modules a NETCONF server implements and download their
// sources (RFC 6022 get-schema) into a local cache directory.
#pragma once

#include <QMetaType>

#include "core/NetconfXml.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ys::netconf {

class Session;

struct DeviceModule {
    QString name, revision, ns;
    QStringList features, deviations;
    bool implemented = true; ///< false for yang-library import-only modules
};

struct SchemaSyncResult {
    QString cacheDir;
    QVector<DeviceModule> modules;   ///< what the device implements
    QString moduleSource;            ///< "hello", "yang-library 1.0", "yang-library 1.1"
    QStringList downloaded, cached, failed;
    QStringList messages;
    bool canDownload = false;        ///< server supports ietf-netconf-monitoring
};

/// Progress: (done, total, current identifier). Return false to cancel.
using SyncProgress = std::function<bool(int, int, const QString &)>;

/// Determine the module set and fetch missing schema files into cacheDir.
SchemaSyncResult syncSchemas(Session &session, const QString &cacheDir,
                             const SyncProgress &progress = {});

// Exposed for tests --------------------------------------------------------
/// Safe file name for a schema: only [A-Za-z0-9._-], "name@rev.yang".
QString schemaFileName(const QString &identifier, const QString &version);
QVector<DeviceModule> parseYangLibrary(const QByteArray &data);
QVector<DeviceModule> modulesFromHello(const QStringList &capabilities);

inline const QString kYangLibraryNs = QStringLiteral("urn:ietf:params:xml:ns:yang:ietf-yang-library");
inline const QString kYangLibrary10 = QStringLiteral("urn:ietf:params:netconf:capability:yang-library:1.0");
inline const QString kYangLibrary11 = QStringLiteral("urn:ietf:params:netconf:capability:yang-library:1.1");

} // namespace ys::netconf

Q_DECLARE_METATYPE(ys::netconf::SchemaSyncResult)
