// SPDX-License-Identifier: GPL-3.0-or-later
// Plain data types for gNMI, independent of protobuf/gRPC so the GUI and the
// conversion code build without them.
#pragma once

#include "core/GnmiPath.h"

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ys::gnmi {

enum class Encoding { JSON = 0, Bytes = 1, Proto = 2, Ascii = 3, JsonIetf = 4 };
QString encodingName(Encoding e);

enum class TlsMode {
    Insecure,   ///< plain text gRPC (labs only)
    Tls,        ///< TLS, server verified against the CA file or the system roots
};

struct ConnectParams {
    QString host;
    int port = 57400;
    TlsMode tls = TlsMode::Tls;
    QString caFile;           ///< PEM; empty = system roots
    QString certFile;         ///< client certificate (mTLS), PEM
    QString keyFile;          ///< client key (mTLS), PEM
    QString serverName;       ///< TLS name to verify instead of host (ssl_target_name_override)
    QString username;         ///< sent as "username"/"password" call metadata
    QString password;
    int timeoutSec = 15;
};

/// A value as received: scalar text for display, or JSON for json/json_ietf.
struct Value {
    enum Kind { None, String, Int, Uint, Bool, Bytes, Double, Decimal, LeafList, Json, JsonIetf, Ascii, Proto, Any };
    Kind kind = None;
    QString text;             ///< display text (scalars; JSON compact text)
    QByteArray json;          ///< raw JSON for Json/JsonIetf
    QStringList list;         ///< leaf-list elements
    bool isJson() const { return kind == Json || kind == JsonIetf; }
    QString kindName() const;
};

struct Update {
    Path path;                ///< full path (notification prefix applied)
    Value value;
    quint32 duplicates = 0;
};

struct Notification {
    qint64 timestampNs = 0;
    Path prefix;
    QVector<Update> updates;
    QVector<Path> deletes;    ///< full paths
    bool atomic = false;
};

struct ModelData {
    QString name, organization, version;
};

struct Capabilities {
    QVector<ModelData> models;
    QVector<Encoding> encodings;
    QString version;
};

enum class DataType { All = 0, Config = 1, State = 2, Operational = 3 };

struct SetOperation {
    enum Kind { Update, Replace, Delete };
    Kind kind = Update;
    Path path;
    QByteArray json;          ///< JSON_IETF value (empty for Delete)
    int line = 0;             ///< line in the editor, for messages
};

struct SetResult {
    qint64 timestampNs = 0;
    QStringList results;      ///< "UPDATE /path" etc.
};

enum class SubscriptionMode { TargetDefined = 0, OnChange = 1, Sample = 2 };
enum class ListMode { Stream = 0, Once = 1, Poll = 2 };

struct SubscriptionSpec {
    Path path;
    SubscriptionMode mode = SubscriptionMode::Sample;
    quint64 sampleIntervalNs = 10'000'000'000ULL;
    bool suppressRedundant = false;
    quint64 heartbeatIntervalNs = 0;
};

struct SubscribeSpec {
    Path prefix;
    QVector<SubscriptionSpec> subscriptions;
    ListMode mode = ListMode::Stream;
    Encoding encoding = Encoding::JsonIetf;
    bool updatesOnly = false;
};

} // namespace ys::gnmi

Q_DECLARE_METATYPE(ys::gnmi::ConnectParams)
Q_DECLARE_METATYPE(ys::gnmi::Notification)
Q_DECLARE_METATYPE(ys::gnmi::Capabilities)
Q_DECLARE_METATYPE(ys::gnmi::SetResult)
Q_DECLARE_METATYPE(ys::gnmi::SubscribeSpec)
Q_DECLARE_METATYPE(QVector<ys::gnmi::Notification>)
Q_DECLARE_METATYPE(QVector<ys::gnmi::SetOperation>)
