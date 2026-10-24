// SPDX-License-Identifier: GPL-3.0-or-later
// Blocking NETCONF-over-SSH client session (RFC 6241 / RFC 6242) on libssh.
// Meant to be driven from a worker thread; one RPC at a time.
#pragma once

#include <QMetaType>

#include "core/NetconfFraming.h"
#include "core/NetconfXml.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

struct ssh_session_struct;
struct ssh_channel_struct;

namespace ys::netconf {

enum class AuthMethod {
    Password,  ///< password, falling back to keyboard-interactive
    KeyFile,   ///< private key file (optional passphrase)
    Auto       ///< SSH agent and default keys (~/.ssh/id_*)
};

struct ConnectParams {
    QString host;
    int port = 830;
    QString user;
    AuthMethod auth = AuthMethod::Password;
    QString password;      ///< password, or key passphrase
    QString keyFile;
    int timeoutSec = 15;   ///< connect / hello / default RPC timeout
    QString knownHostsFile; ///< empty: ~/.ssh/known_hosts (as resolved by libssh)
};

enum class HostKeyStatus { Unknown, NotFound, Changed, OtherType, Error };

/// Called when the server's host key is not in known_hosts. Return true to
/// trust it; @p save controls whether it is written to known_hosts.
using HostKeyPrompt = std::function<bool(const QString &host, const QString &fingerprint,
                                         HostKeyStatus status, bool *save)>;

class Session {
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool open(const ConnectParams &p, const HostKeyPrompt &prompt);
    /// Send one operation and wait for its reply. The raw reply XML is
    /// stored in *rawReply (if given).
    bool rpc(const QByteArray &operation, Reply *reply, QByteArray *rawReply = nullptr,
             int timeoutMs = -1);
    /// Send <close-session/> (best effort) and disconnect.
    void close();
    bool isOpen() const { return m_channel != nullptr; }

    /// Abort a blocking open() or rpc() from another thread.
    void cancel() { m_cancel = true; }

    QString lastError() const { return m_error; }
    const Hello &serverHello() const { return m_hello; }
    bool chunkedFraming() const { return m_decoder.chunked(); }
    QString lastMessageId() const { return QString::number(m_nextId - 1); }
    QString hostKeyFingerprint() const { return m_fingerprint; }

private:
    bool fail(const QString &msg);
    bool checkHostKey(const ConnectParams &p, const HostKeyPrompt &prompt);
    bool authenticate(const ConnectParams &p);
    bool writeAll(const QByteArray &data);
    bool readMessage(QByteArray *msg, int timeoutMs);
    void teardown();

    ssh_session_struct *m_ssh = nullptr;
    ssh_channel_struct *m_channel = nullptr;
    FrameDecoder m_decoder;
    Hello m_hello;
    QString m_error;
    QString m_fingerprint;
    quint64 m_nextId = 1;
    int m_defaultTimeoutMs = 15000;
    std::atomic<bool> m_cancel{false};
};

} // namespace ys::netconf

Q_DECLARE_METATYPE(ys::netconf::ConnectParams)
