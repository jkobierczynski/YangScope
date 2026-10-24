// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/NetconfWorker.h"

#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>

namespace ys {

using namespace ys::netconf;

HostKeyAsker::HostKeyAsker(QWidget *dialogParent) : m_parent(dialogParent) {}

int HostKeyAsker::ask(const QString &host, const QString &fingerprint, int status)
{
    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Unknown Host Key"));
    const QString why = HostKeyStatus(status) == HostKeyStatus::NotFound
                            ? tr("There is no known_hosts file yet.")
                            : tr("This host is not in your known_hosts file.");
    box.setText(tr("<p>The authenticity of <b>%1</b> can't be established.</p>"
                   "<p>%2</p><p>Host key fingerprint:<br><code>%3</code></p>"
                   "<p>Compare it with the device (for example <i>show ssh host-key</i>) "
                   "before you continue.</p>")
                    .arg(host.toHtmlEscaped(), why, fingerprint.toHtmlEscaped()));
    QPushButton *save = box.addButton(tr("Trust and Remember"), QMessageBox::AcceptRole);
    QPushButton *once = box.addButton(tr("Trust Once"), QMessageBox::YesRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == static_cast<QAbstractButton *>(save))
        return 2;
    if (box.clickedButton() == static_cast<QAbstractButton *>(once))
        return 1;
    return 0;
}

NetconfWorker::NetconfWorker(HostKeyAsker *asker) : m_asker(asker) {}

void NetconfWorker::open(const ConnectParams &params)
{
    HostKeyAsker *asker = m_asker;
    const HostKeyPrompt prompt = [asker](const QString &host, const QString &fp,
                                         HostKeyStatus st, bool *save) {
        int answer = 0;
        QMetaObject::invokeMethod(asker, "ask", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(int, answer), Q_ARG(QString, host),
                                  Q_ARG(QString, fp), Q_ARG(int, int(st)));
        *save = answer == 2;
        return answer > 0;
    };
    if (!m_session.open(params, prompt)) {
        emit openFailed(m_session.lastError());
        return;
    }
    // lastError() may hold a non-fatal warning (known_hosts not writable)
    emit opened(m_session.serverHello().capabilities, m_session.serverHello().sessionId,
                m_session.chunkedFraming(), m_session.hostKeyFingerprint(), m_session.lastError());
}

void NetconfWorker::rpc(int requestId, const QString &label, const QByteArray &operation)
{
    QElapsedTimer t;
    t.start();
    Reply reply;
    QByteArray raw;
    const bool ok = m_session.rpc(operation, &reply, &raw);
    emit rpcFinished(requestId, label, ok, reply, raw, t.elapsed(),
                     ok ? QString() : m_session.lastError());
    if (!m_session.isOpen())
        emit closed(m_session.lastError());
}

void NetconfWorker::syncSchemas(const QString &cacheDir)
{
    const SchemaSyncResult res = netconf::syncSchemas(
        m_session, cacheDir, [this](int done, int total, const QString &cur) {
            emit schemaProgress(done, total, cur);
            return true;
        });
    emit schemasSynced(res);
    if (!m_session.isOpen())
        emit closed(m_session.lastError());
}

void NetconfWorker::close()
{
    const bool wasOpen = m_session.isOpen();
    m_session.close();
    if (wasOpen)
        emit closed(QString());
}

NetconfClient::NetconfClient(QWidget *dialogParent)
    : QObject(dialogParent), m_asker(new HostKeyAsker(dialogParent))
{
    qRegisterMetaType<Reply>();
    qRegisterMetaType<SchemaSyncResult>();
    qRegisterMetaType<ConnectParams>();
    m_asker->setParent(this);
    m_worker = new NetconfWorker(m_asker);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("netconf"));
    m_thread.start();
}

NetconfClient::~NetconfClient()
{
    m_worker->cancel();
    QMetaObject::invokeMethod(m_worker, &NetconfWorker::close, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait(5000);
}

} // namespace ys
