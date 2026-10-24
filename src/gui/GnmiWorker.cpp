// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/GnmiWorker.h"

#include <QElapsedTimer>

namespace ys {

using namespace ys::gnmi;

void GnmiCallWorker::open(const ConnectParams &params)
{
    if (!m_client->connect(params)) {
        emit openFailed(m_client->lastError());
        return;
    }
    Capabilities caps;
    if (!m_client->capabilities(&caps)) {
        const QString err = m_client->lastError();
        m_client->disconnect();
        emit openFailed(err);
        return;
    }
    emit opened(caps);
}

void GnmiCallWorker::get(int id, const QStringList &paths, int type, int encoding)
{
    QElapsedTimer t;
    t.start();
    QVector<Path> ps;
    for (const QString &s : paths) {
        Path p;
        QString err;
        if (!parsePath(s, &p, &err)) {
            emit getFinished(id, false, {}, 0, QStringLiteral("%1: %2").arg(s, err));
            return;
        }
        ps << p;
    }
    QVector<Notification> notes;
    const bool ok = m_client->get(Path(), ps, DataType(type), Encoding(encoding), &notes);
    emit getFinished(id, ok, notes, t.elapsed(), ok ? QString() : m_client->lastError());
}

void GnmiCallWorker::set(int id, const QVector<SetOperation> &ops)
{
    QElapsedTimer t;
    t.start();
    SetResult res;
    const bool ok = m_client->set(ops, &res);
    emit setFinished(id, ok, res, t.elapsed(), ok ? QString() : m_client->lastError());
}

void GnmiCallWorker::close()
{
    const bool was = m_client->isConnected();
    m_client->disconnect();
    if (was)
        emit closed();
}

void GnmiStreamWorker::subscribe(const SubscribeSpec &spec)
{
    const bool ok = m_client->subscribe(
        spec, [this](const Notification &n) { emit update(n); }, [this] { emit synced(); });
    emit finished(ok, ok ? QString() : m_client->lastError());
}

GnmiConnection::GnmiConnection(QObject *parent)
    : QObject(parent), m_client(std::make_shared<Client>())
{
    qRegisterMetaType<ConnectParams>();
    qRegisterMetaType<Notification>();
    qRegisterMetaType<QVector<Notification>>();
    qRegisterMetaType<Capabilities>();
    qRegisterMetaType<SetResult>();
    qRegisterMetaType<SubscribeSpec>();
    qRegisterMetaType<QVector<SetOperation>>();
    m_calls = new GnmiCallWorker(m_client);
    m_stream = new GnmiStreamWorker(m_client);
    m_calls->moveToThread(&m_callThread);
    m_stream->moveToThread(&m_streamThread);
    connect(&m_callThread, &QThread::finished, m_calls, &QObject::deleteLater);
    connect(&m_streamThread, &QThread::finished, m_stream, &QObject::deleteLater);
    m_callThread.setObjectName(QStringLiteral("gnmi-calls"));
    m_streamThread.setObjectName(QStringLiteral("gnmi-stream"));
    m_callThread.start();
    m_streamThread.start();
}

GnmiConnection::~GnmiConnection()
{
    m_client->cancelSubscription();
    m_client->cancelCall();
    m_streamThread.quit();
    m_streamThread.wait(5000);
    QMetaObject::invokeMethod(m_calls, &GnmiCallWorker::close, Qt::BlockingQueuedConnection);
    m_callThread.quit();
    m_callThread.wait(5000);
}

} // namespace ys
