// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NetconfSession.h"

#include <libssh/libssh.h>

#include <QElapsedTimer>

#include <mutex>

namespace ys::netconf {

namespace {

constexpr int kReadSliceMs = 200; // granularity for cancel()

void initLibsshOnce()
{
    static std::once_flag once;
    std::call_once(once, [] { ssh_init(); });
}

QString sshError(ssh_session s)
{
    return QString::fromUtf8(ssh_get_error(s));
}

} // namespace

Session::Session()
{
    initLibsshOnce();
}

Session::~Session()
{
    teardown();
}

bool Session::fail(const QString &msg)
{
    m_error = msg;
    teardown();
    return false;
}

void Session::teardown()
{
    if (m_channel) {
        ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }
    if (m_ssh) {
        ssh_disconnect(m_ssh);
        ssh_free(m_ssh);
        m_ssh = nullptr;
    }
}

bool Session::open(const ConnectParams &p, const HostKeyPrompt &prompt)
{
    teardown();
    m_cancel = false;
    m_error.clear();
    m_decoder = FrameDecoder();
    m_hello = Hello();
    m_nextId = 1;
    m_defaultTimeoutMs = qMax(1, p.timeoutSec) * 1000;

    m_ssh = ssh_new();
    if (!m_ssh)
        return fail(QStringLiteral("could not create SSH session"));

    const QByteArray host = p.host.toUtf8();
    const QByteArray user = p.user.toUtf8();
    unsigned int port = unsigned(p.port);
    long timeout = p.timeoutSec;
    ssh_options_set(m_ssh, SSH_OPTIONS_HOST, host.constData());
    // Honour ~/.ssh/config (HostName, Port, IdentityFile, ...) like ssh does,
    // then apply what the user entered on top.
    ssh_options_parse_config(m_ssh, nullptr);
    ssh_options_set(m_ssh, SSH_OPTIONS_PORT, &port);
    if (!user.isEmpty())
        ssh_options_set(m_ssh, SSH_OPTIONS_USER, user.constData());
    ssh_options_set(m_ssh, SSH_OPTIONS_TIMEOUT, &timeout);
    const QByteArray knownHosts = p.knownHostsFile.toUtf8();
    if (!knownHosts.isEmpty())
        ssh_options_set(m_ssh, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData());

    if (ssh_connect(m_ssh) != SSH_OK)
        return fail(QStringLiteral("SSH connect to %1:%2 failed: %3")
                        .arg(p.host).arg(p.port).arg(sshError(m_ssh)));
    if (m_cancel)
        return fail(QStringLiteral("cancelled"));
    if (!checkHostKey(p, prompt))
        return false;
    if (!authenticate(p))
        return false;

    m_channel = ssh_channel_new(m_ssh);
    if (!m_channel || ssh_channel_open_session(m_channel) != SSH_OK)
        return fail(QStringLiteral("could not open SSH channel: ") + sshError(m_ssh));
    if (ssh_channel_request_subsystem(m_channel, "netconf") != SSH_OK)
        return fail(QStringLiteral("server refused the netconf subsystem: ") + sshError(m_ssh));

    // Hello exchange: both sides send at once, framing is end-of-message.
    if (!writeAll(encodeFrame(clientHello(), false)))
        return false;
    QByteArray helloXml;
    if (!readMessage(&helloXml, m_defaultTimeoutMs))
        return fail(m_error.isEmpty() ? QStringLiteral("no hello from server") : m_error);
    m_hello = parseHello(helloXml);
    if (!m_hello.ok)
        return fail(m_hello.error);
    if (!m_hello.capabilities.contains(kBase10) && !m_hello.capabilities.contains(kBase11))
        return fail(QStringLiteral("server supports neither NETCONF base:1.0 nor base:1.1"));
    m_decoder.setChunked(m_hello.capabilities.contains(kBase11));
    return true;
}

bool Session::checkHostKey(const ConnectParams &p, const HostKeyPrompt &prompt)
{
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(m_ssh, &key) != SSH_OK)
        return fail(QStringLiteral("could not read the server host key"));
    unsigned char *hash = nullptr;
    size_t hlen = 0;
    const int rc = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
    ssh_key_free(key);
    if (rc != 0)
        return fail(QStringLiteral("could not hash the server host key"));
    char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    m_fingerprint = QString::fromUtf8(fp);
    ssh_string_free_char(fp);
    ssh_clean_pubkey_hash(&hash);

    const ssh_known_hosts_e state = ssh_session_is_known_server(m_ssh);
    switch (state) {
    case SSH_KNOWN_HOSTS_OK:
        return true;
    case SSH_KNOWN_HOSTS_CHANGED:
    case SSH_KNOWN_HOSTS_OTHER:
        // Never continue: this is what a man-in-the-middle looks like.
        return fail(QStringLiteral("host key for %1 does not match known_hosts (%2). "
                                   "Remove the old entry if the device was replaced.")
                        .arg(p.host, m_fingerprint));
    case SSH_KNOWN_HOSTS_NOT_FOUND:
    case SSH_KNOWN_HOSTS_UNKNOWN: {
        bool save = false;
        const HostKeyStatus st = state == SSH_KNOWN_HOSTS_NOT_FOUND ? HostKeyStatus::NotFound
                                                                    : HostKeyStatus::Unknown;
        if (!prompt || !prompt(p.host, m_fingerprint, st, &save))
            return fail(QStringLiteral("host key not trusted"));
        if (save && ssh_session_update_known_hosts(m_ssh) != SSH_OK)
            m_error = QStringLiteral("warning: could not update known_hosts: ") + sshError(m_ssh);
        return true;
    }
    case SSH_KNOWN_HOSTS_ERROR:
    default:
        return fail(QStringLiteral("known_hosts check failed: ") + sshError(m_ssh));
    }
}

bool Session::authenticate(const ConnectParams &p)
{
    int rc = ssh_userauth_none(m_ssh, nullptr);
    if (rc == SSH_AUTH_SUCCESS)
        return true;
    if (rc == SSH_AUTH_ERROR)
        return fail(QStringLiteral("authentication error: ") + sshError(m_ssh));
    const int methods = ssh_userauth_list(m_ssh, nullptr);
    const QByteArray secret = p.password.toUtf8();

    switch (p.auth) {
    case AuthMethod::Password: {
        if (methods & SSH_AUTH_METHOD_PASSWORD) {
            rc = ssh_userauth_password(m_ssh, nullptr, secret.constData());
            if (rc == SSH_AUTH_SUCCESS)
                return true;
        }
        if (methods & SSH_AUTH_METHOD_INTERACTIVE) {
            // Many network devices only offer keyboard-interactive; answer
            // every prompt with the password.
            rc = ssh_userauth_kbdint(m_ssh, nullptr, nullptr);
            for (int round = 0; rc == SSH_AUTH_INFO && round < 5; ++round) {
                const int n = ssh_userauth_kbdint_getnprompts(m_ssh);
                for (int i = 0; i < n; ++i)
                    ssh_userauth_kbdint_setanswer(m_ssh, unsigned(i), secret.constData());
                rc = ssh_userauth_kbdint(m_ssh, nullptr, nullptr);
            }
            if (rc == SSH_AUTH_SUCCESS)
                return true;
        }
        if (!(methods & (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_INTERACTIVE)))
            return fail(QStringLiteral("server does not accept password authentication"));
        return fail(QStringLiteral("authentication failed for user %1").arg(p.user));
    }
    case AuthMethod::KeyFile: {
        ssh_key key = nullptr;
        const QByteArray path = p.keyFile.toUtf8();
        if (ssh_pki_import_privkey_file(path.constData(),
                                        secret.isEmpty() ? nullptr : secret.constData(),
                                        nullptr, nullptr, &key) != SSH_OK)
            return fail(QStringLiteral("could not read private key %1 (wrong passphrase?)")
                            .arg(p.keyFile));
        rc = ssh_userauth_publickey(m_ssh, nullptr, key);
        ssh_key_free(key);
        if (rc == SSH_AUTH_SUCCESS)
            return true;
        return fail(QStringLiteral("public key authentication failed for user %1").arg(p.user));
    }
    case AuthMethod::Auto:
        rc = ssh_userauth_publickey_auto(m_ssh, nullptr,
                                         secret.isEmpty() ? nullptr : secret.constData());
        if (rc == SSH_AUTH_SUCCESS)
            return true;
        return fail(QStringLiteral("no SSH agent key or default key was accepted for user %1")
                        .arg(p.user));
    }
    return fail(QStringLiteral("unsupported authentication method"));
}

bool Session::writeAll(const QByteArray &data)
{
    qsizetype off = 0;
    while (off < data.size()) {
        const int n = ssh_channel_write(m_channel, data.constData() + off,
                                        uint32_t(data.size() - off));
        if (n == SSH_ERROR)
            return fail(QStringLiteral("write failed: ") + sshError(m_ssh));
        off += n;
    }
    return true;
}

bool Session::readMessage(QByteArray *msg, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    char buf[65536];
    for (;;) {
        if (m_decoder.next(msg))
            return true;
        if (m_decoder.hasError()) {
            m_error = m_decoder.error();
            return false;
        }
        if (m_cancel) {
            m_error = QStringLiteral("cancelled");
            return false;
        }
        const qint64 left = timeoutMs - t.elapsed();
        if (left <= 0) {
            m_error = QStringLiteral("timed out after %1 s waiting for the server").arg(timeoutMs / 1000.0);
            return false;
        }
        const int n = ssh_channel_read_timeout(m_channel, buf, sizeof buf, 0,
                                               int(qMin<qint64>(left, kReadSliceMs)));
        if (n == SSH_ERROR) {
            m_error = QStringLiteral("read failed: ") + sshError(m_ssh);
            return false;
        }
        if (n > 0)
            m_decoder.feed(QByteArray(buf, n));
        else if (ssh_channel_is_eof(m_channel)) {
            m_error = QStringLiteral("server closed the NETCONF session");
            return false;
        }
    }
}

bool Session::rpc(const QByteArray &operation, Reply *reply, QByteArray *rawReply, int timeoutMs)
{
    if (!isOpen()) {
        m_error = QStringLiteral("not connected");
        return false;
    }
    m_cancel = false;
    const QString id = QString::number(m_nextId++);
    if (!writeAll(encodeFrame(wrapRpc(id, operation), m_decoder.chunked())))
        return false;
    const int to = timeoutMs > 0 ? timeoutMs : m_defaultTimeoutMs;
    for (;;) {
        QByteArray xml;
        if (!readMessage(&xml, to)) {
            // A timed-out or broken exchange leaves the stream in an unknown
            // state; drop the session rather than mis-pairing replies later.
            const QString err = m_error;
            teardown();
            m_error = err;
            return false;
        }
        Reply r = parseReply(xml);
        if (!r.parsed) {
            // e.g. a notification; not ours
            if (!r.parseError.startsWith(QLatin1String("not an rpc-reply"))) {
                m_error = r.parseError;
                return false;
            }
            continue;
        }
        if (!r.messageId.isEmpty() && r.messageId != id)
            continue; // stale reply
        if (rawReply)
            *rawReply = xml;
        if (reply)
            *reply = r;
        if (!r.parseError.isEmpty()) {
            m_error = r.parseError;
            return false;
        }
        return true;
    }
}

void Session::close()
{
    if (isOpen()) {
        Reply r;
        rpc(closeSessionOp(), &r, nullptr, 3000);
    }
    teardown();
}

} // namespace ys::netconf
