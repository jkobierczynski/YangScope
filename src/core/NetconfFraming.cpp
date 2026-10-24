// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NetconfFraming.h"

namespace ys::netconf {

namespace {
const QByteArray kEom = QByteArrayLiteral("]]>]]>");
// RFC 6242: chunk-size is 1..4294967295, so at most 10 digits.
constexpr int kMaxSizeDigits = 10;
} // namespace

QByteArray encodeFrame(const QByteArray &message, bool chunked)
{
    if (!chunked)
        return message + kEom;
    QByteArray out;
    out.reserve(message.size() + 32);
    out += "\n#";
    out += QByteArray::number(message.size());
    out += '\n';
    out += message;
    out += "\n##\n";
    return out;
}

bool FrameDecoder::next(QByteArray *message)
{
    if (hasError())
        return false;
    return m_chunked ? nextChunked(message) : nextEom(message);
}

bool FrameDecoder::nextEom(QByteArray *message)
{
    const qsizetype i = m_buf.indexOf(kEom);
    if (i < 0)
        return false;
    *message = m_buf.left(i);
    m_buf.remove(0, i + kEom.size());
    return true;
}

bool FrameDecoder::nextChunked(QByteArray *message)
{
    // Parse without consuming until a whole message (ending in \n##\n) is here.
    QByteArray out;
    qsizetype pos = 0;
    for (;;) {
        if (m_buf.size() - pos < 4)
            return false;
        if (m_buf.at(pos) != '\n' || m_buf.at(pos + 1) != '#') {
            m_error = QStringLiteral("chunked framing: expected chunk header");
            return false;
        }
        if (m_buf.at(pos + 2) == '#') {
            if (m_buf.at(pos + 3) != '\n') {
                m_error = QStringLiteral("chunked framing: bad end-of-chunks marker");
                return false;
            }
            if (pos == 0) {
                m_error = QStringLiteral("chunked framing: message without chunks");
                return false;
            }
            m_buf.remove(0, pos + 4);
            *message = out;
            return true;
        }
        // "\n#<size>\n"
        qsizetype p = pos + 2;
        quint64 size = 0;
        int digits = 0;
        while (p < m_buf.size() && m_buf.at(p) >= '0' && m_buf.at(p) <= '9') {
            if (++digits > kMaxSizeDigits) {
                m_error = QStringLiteral("chunked framing: chunk size too large");
                return false;
            }
            size = size * 10 + quint64(m_buf.at(p) - '0');
            ++p;
        }
        if (p >= m_buf.size())
            return false; // header incomplete
        if (digits == 0 || m_buf.at(p) != '\n' || size == 0 || m_buf.at(pos + 2) == '0') {
            m_error = QStringLiteral("chunked framing: invalid chunk size");
            return false;
        }
        const qsizetype start = p + 1;
        if (quint64(m_buf.size() - start) < size)
            return false; // chunk incomplete
        out += m_buf.mid(start, qsizetype(size));
        pos = start + qsizetype(size);
    }
}

} // namespace ys::netconf
