// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GnmiClient.h"

#include "proto/gnmi/gnmi.grpc.pb.h"

// Deprecated gNMI fields (Path.element, float_val, decimal_val) are still
// read on purpose: older devices send them.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

#include <grpcpp/grpcpp.h>

#include <QFile>

#include <algorithm>
#include <chrono>

namespace ys::gnmi {

namespace pb = ::gnmi;

namespace {

constexpr int kMaxMessageBytes = 256 * 1024 * 1024;

std::string utf8(const QString &s)
{
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
}

QString qs(const std::string &s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

bool readFile(const QString &path, std::string *out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
        return false;
    }
    const QByteArray b = f.readAll();
    out->assign(b.constData(), size_t(b.size()));
    return true;
}

Path fromProto(const pb::Path &p)
{
    Path out;
    out.origin = qs(p.origin());
    out.target = qs(p.target());
    for (const pb::PathElem &e : p.elem()) {
        PathElem pe;
        pe.name = qs(e.name());
        // protobuf maps are unordered; sort for a stable display
        std::vector<std::pair<std::string, std::string>> keys(e.key().begin(), e.key().end());
        std::sort(keys.begin(), keys.end());
        for (const auto &k : keys)
            pe.keys.append({qs(k.first), qs(k.second)});
        out.elems.append(pe);
    }
    if (p.elem_size() == 0) {
        // deprecated string elements ("interface[name=eth0]")
        for (const std::string &s : p.element()) {
            Path one;
            if (parsePath(QLatin1Char('/') + qs(s), &one) && one.elems.size() == 1)
                out.elems.append(one.elems.first());
            else
                out.elems.append(PathElem{qs(s), {}});
        }
    }
    return out;
}

void toProto(const Path &p, pb::Path *out)
{
    out->Clear();
    if (!p.origin.isEmpty())
        out->set_origin(utf8(p.origin));
    if (!p.target.isEmpty())
        out->set_target(utf8(p.target));
    for (const PathElem &e : p.elems) {
        pb::PathElem *pe = out->add_elem();
        pe->set_name(utf8(e.name));
        for (const auto &k : e.keys)
            (*pe->mutable_key())[utf8(k.first)] = utf8(k.second);
    }
}


Value fromProto(const pb::TypedValue &v)
{
    Value out;
    switch (v.value_case()) {
    case pb::TypedValue::kStringVal: out.kind = Value::String; out.text = qs(v.string_val()); break;
    case pb::TypedValue::kIntVal: out.kind = Value::Int; out.text = QString::number(v.int_val()); break;
    case pb::TypedValue::kUintVal: out.kind = Value::Uint; out.text = QString::number(v.uint_val()); break;
    case pb::TypedValue::kBoolVal:
        out.kind = Value::Bool;
        out.text = v.bool_val() ? QStringLiteral("true") : QStringLiteral("false");
        break;
    case pb::TypedValue::kBytesVal:
        out.kind = Value::Bytes;
        out.text = QString::fromLatin1(QByteArray::fromStdString(v.bytes_val()).toBase64());
        break;
    case pb::TypedValue::kFloatVal:
        out.kind = Value::Double;
        out.text = QString::number(double(v.float_val()), 'g', 9);
        break;
    case pb::TypedValue::kDoubleVal:
        out.kind = Value::Double;
        out.text = QString::number(v.double_val(), 'g', 17);
        break;
    case pb::TypedValue::kDecimalVal: {
        out.kind = Value::Decimal;
        const qint64 digits = v.decimal_val().digits();
        const int prec = int(v.decimal_val().precision());
        QString s = QString::number(qAbs(digits));
        while (s.size() <= prec)
            s.prepend(QLatin1Char('0'));
        if (prec > 0)
            s.insert(s.size() - prec, QLatin1Char('.'));
        out.text = (digits < 0 ? QStringLiteral("-") : QString()) + s;
        break;
    }
    case pb::TypedValue::kLeaflistVal:
        out.kind = Value::LeafList;
        for (const pb::TypedValue &e : v.leaflist_val().element())
            out.list << fromProto(e).text;
        out.text = QLatin1Char('[') + out.list.join(QStringLiteral(", ")) + QLatin1Char(']');
        break;
    case pb::TypedValue::kAnyVal:
        out.kind = Value::Any;
        out.text = QStringLiteral("(any: %1)").arg(qs(v.any_val().type_url()));
        break;
    case pb::TypedValue::kJsonVal:
        out.kind = Value::Json;
        out.json = QByteArray::fromStdString(v.json_val());
        out.text = QString::fromUtf8(out.json).simplified();
        break;
    case pb::TypedValue::kJsonIetfVal:
        out.kind = Value::JsonIetf;
        out.json = QByteArray::fromStdString(v.json_ietf_val());
        out.text = QString::fromUtf8(out.json).simplified();
        break;
    case pb::TypedValue::kAsciiVal: out.kind = Value::Ascii; out.text = qs(v.ascii_val()); break;
    case pb::TypedValue::kProtoBytes:
        out.kind = Value::Proto;
        out.text = QStringLiteral("(%1 protobuf bytes)").arg(v.proto_bytes().size());
        break;
    case pb::TypedValue::VALUE_NOT_SET:
        break;
    }
    return out;
}

Notification fromProto(const pb::Notification &n)
{
    Notification out;
    out.timestampNs = n.timestamp();
    out.prefix = fromProto(n.prefix());
    out.atomic = n.atomic();
    for (const pb::Update &u : n.update()) {
        Update up;
        up.path = out.prefix.joined(fromProto(u.path()));
        up.path.origin = !u.path().origin().empty() ? qs(u.path().origin()) : out.prefix.origin;
        up.value = fromProto(u.val());
        up.duplicates = u.duplicates();
        out.updates << up;
    }
    for (const pb::Path &d : n.delete_())
        out.deletes << out.prefix.joined(fromProto(d));
    return out;
}

QString statusText(const grpc::Status &s)
{
    QString msg = Client::statusName(int(s.error_code()));
    if (!s.error_message().empty())
        msg += QStringLiteral(": ") + qs(s.error_message());
    if (s.error_code() == grpc::StatusCode::UNAVAILABLE)
        msg += QStringLiteral(" (is the address right, the port open, and TLS set as the device expects?)");
    else if (s.error_code() == grpc::StatusCode::UNAUTHENTICATED)
        msg += QStringLiteral(" (check username and password)");
    return msg;
}

} // namespace

struct Client::Impl {
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<pb::gNMI::Stub> stub;
    ConnectParams params;

    std::mutex mutex;  // guards the two contexts and the stream pointer
    grpc::ClientContext *callCtx = nullptr;
    grpc::ClientContext *subCtx = nullptr;
    grpc::ClientReaderWriter<pb::SubscribeRequest, pb::SubscribeResponse> *stream = nullptr;
    std::mutex writeMutex;
    bool subCancelled = false;

    void prepare(grpc::ClientContext &ctx, bool unary) const
    {
        if (!params.username.isEmpty()) {
            ctx.AddMetadata("username", utf8(params.username));
            ctx.AddMetadata("password", utf8(params.password));
        }
        if (unary)
            ctx.set_deadline(std::chrono::system_clock::now()
                             + std::chrono::seconds(qMax(5, params.timeoutSec) * 4));
    }
};

Client::Client() : d(std::make_unique<Impl>()) {}

Client::~Client()
{
    cancelSubscription();
    cancelCall();
}

void Client::setError(const QString &e)
{
    std::lock_guard<std::mutex> l(m_errorMutex);
    m_error = e;
}

QString Client::lastError() const
{
    std::lock_guard<std::mutex> l(m_errorMutex);
    return m_error;
}

QString Client::statusName(int code)
{
    static const char *names[] = {"OK", "CANCELLED", "UNKNOWN", "INVALID_ARGUMENT", "DEADLINE_EXCEEDED",
                                  "NOT_FOUND", "ALREADY_EXISTS", "PERMISSION_DENIED", "RESOURCE_EXHAUSTED",
                                  "FAILED_PRECONDITION", "ABORTED", "OUT_OF_RANGE", "UNIMPLEMENTED",
                                  "INTERNAL", "UNAVAILABLE", "DATA_LOSS", "UNAUTHENTICATED"};
    if (code >= 0 && code < int(std::size(names)))
        return QString::fromLatin1(names[code]);
    return QStringLiteral("status %1").arg(code);
}

bool Client::connect(const ConnectParams &params)
{
    disconnect();
    d->params = params;
    m_target = QStringLiteral("%1:%2").arg(params.host.contains(QLatin1Char(':'))
                                               ? QLatin1Char('[') + params.host + QLatin1Char(']')
                                               : params.host)
                   .arg(params.port);
    std::shared_ptr<grpc::ChannelCredentials> creds;
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(kMaxMessageBytes);
    args.SetMaxSendMessageSize(kMaxMessageBytes);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
    if (params.tls == TlsMode::Insecure) {
        creds = grpc::InsecureChannelCredentials();
    } else {
        grpc::SslCredentialsOptions o;
        QString err;
        if (!params.caFile.isEmpty() && !readFile(params.caFile, &o.pem_root_certs, &err)) {
            setError(err);
            return false;
        }
        if (!params.certFile.isEmpty() || !params.keyFile.isEmpty()) {
            if (params.certFile.isEmpty() || params.keyFile.isEmpty()) {
                setError(QStringLiteral("a client certificate needs both the certificate and the key file"));
                return false;
            }
            if (!readFile(params.certFile, &o.pem_cert_chain, &err) || !readFile(params.keyFile, &o.pem_private_key, &err)) {
                setError(err);
                return false;
            }
        }
        creds = grpc::SslCredentials(o);
        if (!params.serverName.isEmpty())
            args.SetSslTargetNameOverride(utf8(params.serverName));
    }
    d->channel = grpc::CreateCustomChannel(utf8(m_target), creds, args);
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(qMax(2, params.timeoutSec));
    if (!d->channel->WaitForConnected(deadline)) {
        const auto state = d->channel->GetState(false);
        setError(QStringLiteral("could not connect to %1 within %2 s%3")
                     .arg(m_target).arg(params.timeoutSec)
                     .arg(params.tls == TlsMode::Insecure
                              ? QStringLiteral(" (insecure mode: does the device require TLS?)")
                              : QStringLiteral(" (TLS: check the CA file, the server name and whether the "
                                               "device uses TLS at all)")));
        Q_UNUSED(state);
        d->channel.reset();
        return false;
    }
    d->stub = pb::gNMI::NewStub(d->channel);
    setError(QString());
    return true;
}

void Client::disconnect()
{
    cancelSubscription();
    cancelCall();
    d->stub.reset();
    d->channel.reset();
}

bool Client::isConnected() const
{
    return d->stub != nullptr;
}

void Client::cancelCall()
{
    std::lock_guard<std::mutex> l(d->mutex);
    if (d->callCtx)
        d->callCtx->TryCancel();
}

void Client::cancelSubscription()
{
    std::lock_guard<std::mutex> l(d->mutex);
    d->subCancelled = true;
    if (d->subCtx)
        d->subCtx->TryCancel();
}

namespace {
struct CallScope {
    std::mutex &m;
    grpc::ClientContext *&slot;
    CallScope(std::mutex &mm, grpc::ClientContext *&s, grpc::ClientContext *ctx) : m(mm), slot(s)
    {
        std::lock_guard<std::mutex> l(m);
        slot = ctx;
    }
    ~CallScope()
    {
        std::lock_guard<std::mutex> l(m);
        slot = nullptr;
    }
};
} // namespace

bool Client::capabilities(Capabilities *out)
{
    if (!d->stub) {
        setError(QStringLiteral("not connected"));
        return false;
    }
    grpc::ClientContext ctx;
    d->prepare(ctx, true);
    CallScope scope(d->mutex, d->callCtx, &ctx);
    pb::CapabilityRequest req;
    pb::CapabilityResponse resp;
    const grpc::Status st = d->stub->Capabilities(&ctx, req, &resp);
    if (!st.ok()) {
        setError(statusText(st));
        return false;
    }
    *out = Capabilities();
    out->version = qs(resp.gnmi_version());
    for (const pb::ModelData &m : resp.supported_models())
        out->models << ModelData{qs(m.name()), qs(m.organization()), qs(m.version())};
    for (int e : resp.supported_encodings())
        out->encodings << Encoding(e);
    return true;
}

bool Client::get(const Path &prefix, const QVector<Path> &paths, DataType type, Encoding encoding,
                 QVector<Notification> *out)
{
    if (!d->stub) {
        setError(QStringLiteral("not connected"));
        return false;
    }
    grpc::ClientContext ctx;
    d->prepare(ctx, true);
    CallScope scope(d->mutex, d->callCtx, &ctx);
    pb::GetRequest req;
    if (!prefix.isRoot() || !prefix.origin.isEmpty() || !prefix.target.isEmpty())
        toProto(prefix, req.mutable_prefix());
    for (const Path &p : paths)
        toProto(p, req.add_path());
    req.set_type(pb::GetRequest::DataType(int(type)));
    req.set_encoding(pb::Encoding(int(encoding)));
    pb::GetResponse resp;
    const grpc::Status st = d->stub->Get(&ctx, req, &resp);
    if (!st.ok()) {
        setError(statusText(st));
        return false;
    }
    out->clear();
    for (const pb::Notification &n : resp.notification())
        out->append(fromProto(n));
    return true;
}

bool Client::set(const QVector<SetOperation> &ops, SetResult *out)
{
    if (!d->stub) {
        setError(QStringLiteral("not connected"));
        return false;
    }
    grpc::ClientContext ctx;
    d->prepare(ctx, true);
    CallScope scope(d->mutex, d->callCtx, &ctx);
    pb::SetRequest req;
    for (const SetOperation &op : ops) {
        if (op.kind == SetOperation::Delete) {
            toProto(op.path, req.add_delete_());
            continue;
        }
        pb::Update *u = op.kind == SetOperation::Replace ? req.add_replace() : req.add_update();
        toProto(op.path, u->mutable_path());
        u->mutable_val()->set_json_ietf_val(op.json.toStdString());
    }
    pb::SetResponse resp;
    const grpc::Status st = d->stub->Set(&ctx, req, &resp);
    if (!st.ok()) {
        setError(statusText(st));
        return false;
    }
    *out = SetResult();
    out->timestampNs = resp.timestamp();
    const Path prefix = fromProto(resp.prefix());
    for (const pb::UpdateResult &r : resp.response()) {
        out->results << QStringLiteral("%1 %2").arg(qs(pb::UpdateResult::Operation_Name(r.op())),
                                                   pathToString(prefix.joined(fromProto(r.path()))));
    }
    return true;
}

bool Client::subscribe(const SubscribeSpec &spec, const UpdateHandler &onUpdate, const SyncHandler &onSync)
{
    if (!d->stub) {
        setError(QStringLiteral("not connected"));
        return false;
    }
    grpc::ClientContext ctx;
    d->prepare(ctx, false);
    {
        std::lock_guard<std::mutex> l(d->mutex);
        d->subCancelled = false;
        d->subCtx = &ctx;
    }
    auto stream = d->stub->Subscribe(&ctx);
    {
        std::lock_guard<std::mutex> l(d->mutex);
        d->stream = stream.get();
    }
    pb::SubscribeRequest req;
    pb::SubscriptionList *list = req.mutable_subscribe();
    if (!spec.prefix.isRoot() || !spec.prefix.origin.isEmpty() || !spec.prefix.target.isEmpty())
        toProto(spec.prefix, list->mutable_prefix());
    list->set_mode(pb::SubscriptionList::Mode(int(spec.mode)));
    list->set_encoding(pb::Encoding(int(spec.encoding)));
    list->set_updates_only(spec.updatesOnly);
    for (const SubscriptionSpec &s : spec.subscriptions) {
        pb::Subscription *sub = list->add_subscription();
        toProto(s.path, sub->mutable_path());
        if (spec.mode == ListMode::Stream) {
            sub->set_mode(pb::SubscriptionMode(int(s.mode)));
            sub->set_sample_interval(s.sampleIntervalNs);
            sub->set_suppress_redundant(s.suppressRedundant);
            sub->set_heartbeat_interval(s.heartbeatIntervalNs);
        }
    }
    bool written;
    {
        std::lock_guard<std::mutex> w(d->writeMutex);
        written = stream->Write(req);
    }
    pb::SubscribeResponse resp;
    if (written) {
        while (stream->Read(&resp)) {
            switch (resp.response_case()) {
            case pb::SubscribeResponse::kUpdate:
                if (onUpdate)
                    onUpdate(fromProto(resp.update()));
                break;
            case pb::SubscribeResponse::kSyncResponse:
                if (onSync)
                    onSync();
                break;
            default:
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> w(d->writeMutex);
        stream->WritesDone();
    }
    const grpc::Status st = stream->Finish();
    bool cancelled;
    {
        std::lock_guard<std::mutex> l(d->mutex);
        d->stream = nullptr;
        d->subCtx = nullptr;
        cancelled = d->subCancelled;
    }
    if (st.ok() || cancelled || st.error_code() == grpc::StatusCode::CANCELLED) {
        setError(QString());
        return true;
    }
    setError(statusText(st));
    return false;
}

bool Client::poll()
{
    std::lock_guard<std::mutex> l(d->mutex);
    if (!d->stream)
        return false;
    pb::SubscribeRequest req;
    req.mutable_poll();
    std::lock_guard<std::mutex> w(d->writeMutex);
    return d->stream->Write(req);
}

} // namespace ys::gnmi
