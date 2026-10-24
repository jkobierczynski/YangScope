// SPDX-License-Identifier: GPL-3.0-or-later
// Blocking gNMI client on gRPC C++. Unary calls and a subscription may run on
// different threads at the same time; cancel() and poll() are thread-safe.
#pragma once

#include "core/GnmiTypes.h"

#include <QString>

#include <functional>
#include <memory>
#include <mutex>

namespace ys::gnmi {

class Client {
public:
    Client();
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    /// Create the channel and wait until it is connected (TLS handshake done).
    bool connect(const ConnectParams &params);
    void disconnect();
    bool isConnected() const;

    bool capabilities(Capabilities *out);
    bool get(const Path &prefix, const QVector<Path> &paths, DataType type, Encoding encoding,
             QVector<Notification> *out);
    bool set(const QVector<SetOperation> &ops, SetResult *out);

    using UpdateHandler = std::function<void(const Notification &)>;
    using SyncHandler = std::function<void()>;
    /// Runs until the server ends the stream (ONCE), cancelSubscription() is
    /// called, or an error happens. Returns false on error (not on cancel).
    bool subscribe(const SubscribeSpec &spec, const UpdateHandler &onUpdate, const SyncHandler &onSync);
    /// Send a Poll on a running POLL subscription.
    bool poll();
    void cancelSubscription();
    /// Cancel the running unary call, if any.
    void cancelCall();

    QString lastError() const;
    QString target() const { return m_target; }

    /// Message for a gRPC status code, e.g. "UNAVAILABLE".
    static QString statusName(int code);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    QString m_target;
    mutable std::mutex m_errorMutex;
    QString m_error;
    void setError(const QString &e);
};

} // namespace ys::gnmi
