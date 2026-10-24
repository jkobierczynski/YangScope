// SPDX-License-Identifier: GPL-3.0-or-later
// Runs a NETCONF session on its own thread so the GUI never blocks on I/O.
#pragma once

#include "core/NetconfSession.h"
#include "core/SchemaSync.h"

#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QThread>

class QWidget;


namespace ys {

/// Lives in the GUI thread; the worker calls ask() through a blocking queued
/// connection when a host key is not in known_hosts.
class HostKeyAsker : public QObject {
    Q_OBJECT
public:
    explicit HostKeyAsker(QWidget *dialogParent);
    /// 0 = reject, 1 = accept once, 2 = accept and save to known_hosts
    Q_INVOKABLE int ask(const QString &host, const QString &fingerprint, int status);

private:
    QPointer<QWidget> m_parent;
};

class NetconfWorker : public QObject {
    Q_OBJECT
public:
    explicit NetconfWorker(HostKeyAsker *asker);
    /// Thread-safe: abort the current blocking operation.
    void cancel() { m_session.cancel(); }

public slots:
    void open(const ys::netconf::ConnectParams &params);
    void rpc(int requestId, const QString &label, const QByteArray &operation);
    void syncSchemas(const QString &cacheDir);
    void close();

signals:
    void opened(const QStringList &capabilities, const QString &sessionId, bool chunked,
                const QString &fingerprint, const QString &warning);
    void openFailed(const QString &error);
    void rpcFinished(int requestId, const QString &label, bool ok,
                     const ys::netconf::Reply &reply, const QByteArray &raw,
                     qint64 elapsedMs, const QString &error);
    void schemaProgress(int done, int total, const QString &current);
    void schemasSynced(const ys::netconf::SchemaSyncResult &result);
    void closed(const QString &reason);

private:
    netconf::Session m_session;
    HostKeyAsker *m_asker;
};

/// Owns the worker thread.
class NetconfClient : public QObject {
    Q_OBJECT
public:
    explicit NetconfClient(QWidget *dialogParent);
    ~NetconfClient() override;
    NetconfWorker *worker() const { return m_worker; }
    void cancel() { m_worker->cancel(); }

private:
    QThread m_thread;
    HostKeyAsker *m_asker;
    NetconfWorker *m_worker;
};

} // namespace ys
