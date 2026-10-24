// SPDX-License-Identifier: GPL-3.0-or-later
// NETCONF message framing over SSH (RFC 6242): end-of-message for base:1.0,
// chunked framing for base:1.1.
#pragma once

#include <QByteArray>
#include <QString>

namespace ys::netconf {

class FrameDecoder {
public:
    /// Switch to chunked framing (after both sides advertised base:1.1).
    void setChunked(bool on) { m_chunked = on; }
    bool chunked() const { return m_chunked; }

    /// Append bytes received from the transport.
    void feed(const QByteArray &data) { m_buf += data; }

    /// Extract the next complete message. Returns false if more input is
    /// needed or the stream is malformed (then error() is set).
    bool next(QByteArray *message);

    QString error() const { return m_error; }
    bool hasError() const { return !m_error.isEmpty(); }
    /// Bytes buffered but not yet part of a complete message.
    qsizetype pending() const { return m_buf.size(); }

private:
    bool nextEom(QByteArray *message);
    bool nextChunked(QByteArray *message);

    QByteArray m_buf;
    QString m_error;
    bool m_chunked = false;
};

/// Frame one message for sending.
QByteArray encodeFrame(const QByteArray &message, bool chunked);

} // namespace ys::netconf
