// SPDX-License-Identifier: GPL-3.0-or-later
// gNMI calls on worker threads: one for unary calls (Capabilities, Get, Set),
// one for the subscription stream, so a running subscription never blocks
// a Get or Set.
#pragma once

#include "core/GnmiClient.h"

#include <QObject>
#include <QThread>

#include <memory>

namespace ys {

class GnmiCallWorker : public QObject {
    Q_OBJECT
public:
    explicit GnmiCallWorker(std::shared_ptr<gnmi::Client> client) : m_client(std::move(client)) {}

public slots:
    void open(const ys::gnmi::ConnectParams &params);
    void get(int id, const QStringList &paths, int type, int encoding);
    void set(int id, const QVector<ys::gnmi::SetOperation> &ops);
    void close();

signals:
    void opened(const ys::gnmi::Capabilities &caps);
    void openFailed(const QString &error);
    void getFinished(int id, bool ok, const QVector<ys::gnmi::Notification> &notes, qint64 ms,
                     const QString &error);
    void setFinished(int id, bool ok, const ys::gnmi::SetResult &result, qint64 ms, const QString &error);
    void closed();

private:
    std::shared_ptr<gnmi::Client> m_client;
};

class GnmiStreamWorker : public QObject {
    Q_OBJECT
public:
    explicit GnmiStreamWorker(std::shared_ptr<gnmi::Client> client) : m_client(std::move(client)) {}

public slots:
    void subscribe(const ys::gnmi::SubscribeSpec &spec);

signals:
    void update(const ys::gnmi::Notification &n);
    void synced();
    void finished(bool ok, const QString &error);

private:
    std::shared_ptr<gnmi::Client> m_client;
};

/// Owns the client and both threads.
class GnmiConnection : public QObject {
    Q_OBJECT
public:
    explicit GnmiConnection(QObject *parent = nullptr);
    ~GnmiConnection() override;
    GnmiCallWorker *calls() const { return m_calls; }
    GnmiStreamWorker *stream() const { return m_stream; }
    /// Thread-safe controls of the running subscription / call.
    void cancelSubscription() { m_client->cancelSubscription(); }
    bool poll() { return m_client->poll(); }
    void cancelCall() { m_client->cancelCall(); }

private:
    std::shared_ptr<gnmi::Client> m_client;
    QThread m_callThread;
    QThread m_streamThread;
    GnmiCallWorker *m_calls;
    GnmiStreamWorker *m_stream;
};

} // namespace ys
