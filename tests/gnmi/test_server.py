#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal gNMI server for testing YangScope.

Not a real device: the configuration (--config) and state (--state) are
RFC 7951 JSON documents held in memory. It implements Capabilities, Get, Set
(update / replace / delete) and Subscribe (ONCE, POLL, STREAM with SAMPLE,
ON_CHANGE and TARGET_DEFINED). The eth0 counters grow every second so
streaming subscriptions have something to show. List entries are matched on
the members named in KEYS, which is enough for the test models.

    python3 test_server.py --port 57400 --config config.json --state state.json \\
        [--user admin --password admin] [--tls CERTDIR [--mtls]] [--models ...]

With --tls a CA, a server certificate (localhost, 127.0.0.1) and a client
certificate are written to CERTDIR (ca.pem, server.pem/-key.pem,
client.pem/-key.pem) unless they exist. Python stubs are generated from the
vendored protos at start-up (needs grpcio-tools).
"""
import argparse
import copy
import datetime
import importlib
import json
import logging
import math
import os
import queue
import sys
import tempfile
import threading
import time
from concurrent import futures

import grpc

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_ROOT = os.path.join(HERE, "..", "..", "third_party", "gnmi")
KEYS = ("name", "port", "ip")
# eth0 traffic, per second
RATES = {"in-octets": 1_250_000, "out-octets": 400_000, "in-unicast-pkts": 1100, "out-unicast-pkts": 450}
log = logging.getLogger("gnmi-test-server")


def load_stubs():
    """Generate gnmi_pb2 / gnmi_pb2_grpc into a temp dir and import them."""
    from grpc_tools import protoc
    try:
        import importlib.resources as res
        include = str(res.files("grpc_tools") / "_proto")
    except Exception:  # pragma: no cover - old Python
        import pkg_resources
        include = pkg_resources.resource_filename("grpc_tools", "_proto")
    out = tempfile.mkdtemp(prefix="gnmi-stubs-")
    protos = ["proto/gnmi/gnmi.proto", "proto/gnmi_ext/gnmi_ext.proto"]
    rc = protoc.main(["protoc", f"-I{PROTO_ROOT}", f"-I{include}",
                      f"--python_out={out}", f"--grpc_python_out={out}"] + protos)
    if rc != 0:
        sys.exit(f"protoc failed ({rc})")
    sys.path.insert(0, out)
    pb2 = importlib.import_module("proto.gnmi.gnmi_pb2")
    pb2_grpc = importlib.import_module("proto.gnmi.gnmi_pb2_grpc")
    return pb2, pb2_grpc


pb, pb_grpc = None, None


# --- JSON tree helpers -------------------------------------------------------------

def split(name):
    return name.split(":", 1) if ":" in name else (None, name)


def deep_merge(dst, src):
    """Merge RFC 7951 JSON src into dst (lists merged by KEYS)."""
    for k, v in src.items():
        if k in dst and isinstance(dst[k], dict) and isinstance(v, dict):
            deep_merge(dst[k], v)
        elif k in dst and isinstance(dst[k], list) and isinstance(v, list) and v and isinstance(v[0], dict):
            for entry in v:
                match = find_entry(dst[k], entry_keys(entry))
                if match is None:
                    dst[k].append(copy.deepcopy(entry))
                else:
                    deep_merge(match, entry)
        else:
            dst[k] = copy.deepcopy(v)


def entry_keys(entry):
    return {k: str(v) for k, v in entry.items() if k in KEYS}


def find_entry(lst, keys):
    for e in lst:
        if all(str(e.get(k)) == v for k, v in keys.items()):
            return e
    return None


def member(obj, name, module):
    """Find a member by name (with or without module prefix). Returns
    (key, module_of_member) or (None, None)."""
    want_mod, want = split(name)
    for k in obj:
        mod, local = split(k)
        if local != want:
            continue
        eff = mod or module
        if want_mod and want_mod != eff:
            continue
        return k, eff
    return None, None


def walk(root, elems):
    """Yield (concrete_elems, value, module) for every match of elems
    (wildcard keys '*' allowed)."""
    def rec(node, i, done, module):
        if i == len(elems):
            yield done, node, module
            return
        name, keys = elems[i]
        if not isinstance(node, dict):
            return
        k, mod = member(node, name, module)
        if k is None:
            return
        val = node[k]
        out_name = (f"{mod}:{split(k)[1]}" if i == 0 else split(k)[1]) if mod else split(k)[1]
        if mod != module and i > 0:
            out_name = f"{mod}:{split(k)[1]}"
        if isinstance(val, list) and val and isinstance(val[0], dict):
            for e in val:
                ok = all(v in ("*", "...") or str(e.get(kk)) == v for kk, v in keys.items())
                if ok:
                    concrete = {kk: str(e.get(kk)) for kk in e if kk in KEYS} if keys or True else {}
                    yield from rec(e, i + 1, done + [(out_name, concrete)], mod)
            return
        if keys:
            return
        yield from rec(val, i + 1, done + [(out_name, {})], mod)
    yield from rec(root, 0, [], None)


def leaves(value, base):
    """Flatten a JSON value into (elems, scalar-or-list) pairs."""
    if isinstance(value, dict):
        for k, v in value.items():
            if isinstance(v, list) and v and isinstance(v[0], dict):
                for e in v:
                    keys = {kk: str(vv) for kk, vv in e.items() if kk in KEYS}
                    yield from leaves(e, base + [(k, keys)])
            else:
                yield from leaves(v, base + [(k, {})])
    else:
        yield base, value


def ensure_path(root, elems, module=None):
    """Create containers/list entries for elems (all keys concrete) and return
    (parent_obj, member_key, keys_of_last, module)."""
    node = root
    for i, (name, keys) in enumerate(elems):
        last = i == len(elems) - 1
        k, mod = member(node, name, module)
        if k is None:
            k = name if (":" in name or i > 0) else name
            mod = split(name)[0] or module
            if last and not keys:
                return node, k, keys, mod
            node[k] = [] if keys else {}
        if last:
            return node, k, keys, mod
        val = node[k]
        if keys:
            e = find_entry(val, keys)
            if e is None:
                e = {kk: (int(v) if v.isdigit() else v) for kk, v in keys.items()}
                val.append(e)
            node = e
        else:
            node = val
        module = mod
    raise ValueError("empty path")


def path_elems(path_pb, prefix_pb=None):
    elems = []
    for p in (prefix_pb, path_pb):
        if p is None:
            continue
        for e in p.elem:
            elems.append((e.name, dict(e.key)))
    return elems


def to_path(elems):
    p = pb.Path()
    for name, keys in elems:
        pe = p.elem.add()
        pe.name = name
        for k, v in keys.items():
            pe.key[k] = str(v)
    return p


def typed(value, encoding):
    tv = pb.TypedValue()
    if encoding == pb.JSON_IETF or encoding == pb.JSON or isinstance(value, (dict, list)) and \
            (not isinstance(value, list) or (value and isinstance(value[0], dict))):
        data = json.dumps(value).encode()
        if encoding == pb.JSON:
            tv.json_val = data
        else:
            tv.json_ietf_val = data
        return tv
    if isinstance(value, list):
        for x in value:
            tv.leaflist_val.element.append(typed(x, encoding))
        return tv
    if isinstance(value, bool):
        tv.bool_val = value
    elif isinstance(value, int):
        if value >= 0:
            tv.uint_val = value
        else:
            tv.int_val = value
    elif isinstance(value, float):
        tv.double_val = value
    elif isinstance(value, str) and value.isdigit():
        tv.uint_val = int(value)
    else:
        tv.string_val = str(value)
    return tv


# --- datastore ---------------------------------------------------------------------

class Store:
    def __init__(self, config, state):
        self.lock = threading.RLock()
        self.config = config
        self.state = state
        self.generation = 0
        self.changed = threading.Condition(self.lock)
        self.t0 = time.monotonic()
        self.base = {}
        for e in self.state["ietf-interfaces:interfaces"]["interface"]:
            if e["name"] == "eth0":
                self.base = {k: int(v) for k, v in e["statistics"].items() if k in RATES}

    def update_counters(self):
        """eth0 counters as a smooth function of time: rate * (1 + 0.2 sin(t/3)),
        integrated, so every sample interval sees a sensible rate."""
        t = time.monotonic() - self.t0
        shape = t + 0.6 * (1 - math.cos(t / 3))
        for e in self.state["ietf-interfaces:interfaces"]["interface"]:
            if e["name"] == "eth0":
                for k, r in RATES.items():
                    e["statistics"][k] = str(self.base[k] + int(r * shape))

    def operational(self):
        self.update_counters()
        out = copy.deepcopy(self.config)
        deep_merge(out, self.state)
        return out

    def tree(self, data_type):
        if data_type == pb.GetRequest.CONFIG:
            return copy.deepcopy(self.config)
        if data_type == pb.GetRequest.STATE:
            self.update_counters()
            return copy.deepcopy(self.state)
        return self.operational()

    def bump(self):
        with self.lock:
            self.generation += 1
            self.changed.notify_all()

    def tick(self):
        """Counters moved: wake ON_CHANGE subscriptions."""
        with self.lock:
            self.update_counters()
            self.generation += 1
            self.changed.notify_all()


# --- service --------------------------------------------------------------------------

class GnmiService:
    def __init__(self, args, store):
        self.args = args
        self.store = store

    def check_auth(self, context):
        if not self.args.user:
            return True
        md = dict(context.invocation_metadata())
        if md.get("username") == self.args.user and md.get("password") == self.args.password:
            return True
        context.abort(grpc.StatusCode.UNAUTHENTICATED, "invalid username or password")
        return False

    def Capabilities(self, request, context):
        self.check_auth(context)
        resp = pb.CapabilityResponse(gNMI_version="0.10.0")
        for m in self.args.models:
            name, _, ver = m.partition("@")
            resp.supported_models.add(name=name, organization="YangScope test", version=ver)
        resp.supported_encodings.extend([pb.JSON, pb.JSON_IETF, pb.PROTO])
        return resp

    def collect(self, tree, elems):
        return list(walk(tree, elems)) if elems else \
            [([(k, {})], v, split(k)[0]) for k, v in tree.items()]

    def Get(self, request, context):
        self.check_auth(context)
        if request.encoding not in (pb.JSON, pb.JSON_IETF, pb.PROTO):
            context.abort(grpc.StatusCode.UNIMPLEMENTED, "encoding not supported")
        with self.store.lock:
            tree = self.store.tree(request.type)
        resp = pb.GetResponse()
        now = time.time_ns()
        paths = list(request.path) or [pb.Path()]
        for p in paths:
            elems = path_elems(p, request.prefix if request.HasField("prefix") else None)
            matches = self.collect(tree, elems)
            if not matches:
                context.abort(grpc.StatusCode.NOT_FOUND, f"no data at {fmt(elems)}")
            n = resp.notification.add(timestamp=now)
            for concrete, value, _mod in matches:
                u = n.update.add()
                u.path.CopyFrom(to_path(concrete))
                u.val.CopyFrom(typed(value, request.encoding))
        return resp

    def Set(self, request, context):
        self.check_auth(context)
        resp = pb.SetResponse(timestamp=time.time_ns())
        with self.store.lock:
            work = copy.deepcopy(self.store.config)
            prefix = request.prefix if request.HasField("prefix") else None
            try:
                for d in request.delete:
                    elems = path_elems(d, prefix)
                    self.delete(work, elems)
                    resp.response.add(path=d, op=pb.UpdateResult.DELETE)
                for kind, items in (("replace", request.replace), ("update", request.update)):
                    for u in items:
                        elems = path_elems(u.path, prefix)
                        if u.val.WhichOneof("value") not in ("json_ietf_val", "json_val"):
                            value = scalar_of(u.val)
                        else:
                            value = json.loads(u.val.json_ietf_val or u.val.json_val)
                        self.write(work, elems, value, replace=(kind == "replace"))
                        resp.response.add(path=u.path, op=pb.UpdateResult.REPLACE if kind == "replace"
                                          else pb.UpdateResult.UPDATE)
            except (KeyError, ValueError, TypeError) as e:
                context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(e))
            self.store.config = work
        self.store.bump()
        return resp

    def delete(self, tree, elems):
        if not elems:
            tree.clear()
            return
        for concrete, _v, _m in list(walk(tree, elems)):
            parent, key, keys, _ = ensure_path(tree, concrete)
            if keys:
                parent[key] = [e for e in parent[key]
                               if not all(str(e.get(k)) == v for k, v in keys.items())]
            else:
                parent.pop(key, None)

    def write(self, tree, elems, value, replace):
        if not elems:
            if replace:
                tree.clear()
            deep_merge(tree, value)
            return
        if any(v in ("*", "...") for _, k in elems for v in k.values()):
            raise ValueError("wildcards are not allowed in Set")
        parent, key, keys, _mod = ensure_path(tree, elems)
        if keys:
            entry = find_entry(parent[key], keys)
            new = dict(value) if isinstance(value, dict) else {}
            for k, v in keys.items():
                new.setdefault(k, int(v) if v.isdigit() else v)
            if entry is None:
                parent[key].append(new)
            elif replace:
                parent[key][parent[key].index(entry)] = new
            else:
                deep_merge(entry, new)
        elif isinstance(value, dict) and not replace and isinstance(parent.get(key), dict):
            deep_merge(parent[key], value)
        else:
            parent[key] = copy.deepcopy(value)

    def Subscribe(self, request_iterator, context):
        self.check_auth(context)
        requests = queue.Queue()

        def reader():
            try:
                for r in request_iterator:
                    requests.put(r)
            except Exception:  # client went away
                pass
            requests.put(None)
        threading.Thread(target=reader, daemon=True).start()

        first = requests.get()
        if first is None or not first.HasField("subscribe"):
            context.abort(grpc.StatusCode.INVALID_ARGUMENT, "first message must be a SubscriptionList")
        sl = first.subscribe
        prefix = sl.prefix if sl.HasField("prefix") else None
        enc = sl.encoding

        def snapshot(only_changed=None):
            with self.store.lock:
                tree = self.store.operational()
            out = {}
            for s in sl.subscription:
                for concrete, value, _m in walk(tree, path_elems(s.path, prefix)):
                    for elems, leaf in leaves(value, concrete):
                        out[fmt(elems)] = (elems, leaf)
            return out

        def notification(items):
            n = pb.Notification(timestamp=time.time_ns())
            for elems, leaf in items:
                u = n.update.add()
                u.path.CopyFrom(to_path(elems))
                u.val.CopyFrom(typed(leaf, enc))
            return pb.SubscribeResponse(update=n)

        last = snapshot()
        if not sl.updates_only:
            yield notification(list(last.values()))
        yield pb.SubscribeResponse(sync_response=True)

        if sl.mode == pb.SubscriptionList.ONCE:
            return
        if sl.mode == pb.SubscriptionList.POLL:
            while context.is_active():
                r = requests.get()
                if r is None:
                    return
                if r.HasField("poll"):
                    last = snapshot()
                    yield notification(list(last.values()))
                    yield pb.SubscribeResponse(sync_response=True)
            return

        # STREAM
        sample = [s for s in sl.subscription if s.mode == pb.SAMPLE]
        interval = min([max(s.sample_interval, 200_000_000) for s in sample] or [1_000_000_000]) / 1e9
        on_change = any(s.mode in (pb.ON_CHANGE, pb.TARGET_DEFINED) for s in sl.subscription)
        suppress = all(s.suppress_redundant for s in sample) if sample else False
        gen = self.store.generation
        next_sample = time.monotonic() + interval
        while context.is_active():
            try:
                r = requests.get(timeout=0.05)
                if r is None:
                    return
            except queue.Empty:
                pass
            now = time.monotonic()
            changed_store = self.store.generation != gen
            if sample and now >= next_sample:
                next_sample = now + interval
                cur = snapshot()
                items = [v for k, v in cur.items() if not suppress or last.get(k) != v]
                last = cur
                gen = self.store.generation
                if items:
                    yield notification(items)
            elif on_change and changed_store:
                gen = self.store.generation
                cur = snapshot()
                items = [v for k, v in cur.items() if last.get(k) != v]
                deleted = [k for k in last if k not in cur]
                last = cur
                if items or deleted:
                    resp = notification(items)
                    for k in deleted:
                        resp.update.delete.append(to_path(path_from_fmt(k)))
                    yield resp


def scalar_of(tv):
    kind = tv.WhichOneof("value")
    v = getattr(tv, kind)
    if kind == "leaflist_val":
        return [scalar_of(x) for x in v.element]
    return v


def fmt(elems):
    return "/" + "/".join(n + "".join(f"[{k}={v}]" for k, v in sorted(keys.items()))
                          for n, keys in elems)


def path_from_fmt(s):
    import re
    out = []
    for part in re.findall(r"[^/\[]+(?:\[[^\]]*\])*", s):
        name = part.split("[", 1)[0]
        keys = dict(re.findall(r"\[([^=\]]+)=([^\]]*)\]", part))
        out.append((name, keys))
    return out


# --- certificates --------------------------------------------------------------------

def make_certs(d):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    import ipaddress
    os.makedirs(d, exist_ok=True)
    if os.path.exists(os.path.join(d, "ca.pem")):
        return
    now = datetime.datetime.now(datetime.timezone.utc)

    def key():
        return ec.generate_private_key(ec.SECP256R1())

    def write(name, k, cert):
        with open(os.path.join(d, f"{name}-key.pem"), "wb") as f:
            f.write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                    serialization.NoEncryption()))
        with open(os.path.join(d, f"{name}.pem"), "wb") as f:
            f.write(cert.public_bytes(serialization.Encoding.PEM))

    ca_key = key()
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "YangScope test CA")])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now - datetime.timedelta(days=1))
          .not_valid_after(now + datetime.timedelta(days=3650))
          .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
          .sign(ca_key, hashes.SHA256()))
    with open(os.path.join(d, "ca.pem"), "wb") as f:
        f.write(ca.public_bytes(serialization.Encoding.PEM))

    def leaf(name, cn, san, usage):
        k = key()
        c = (x509.CertificateBuilder()
             .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
             .issuer_name(ca_name).public_key(k.public_key())
             .serial_number(x509.random_serial_number())
             .not_valid_before(now - datetime.timedelta(days=1))
             .not_valid_after(now + datetime.timedelta(days=3650))
             .add_extension(x509.SubjectAlternativeName(san), critical=False)
             .add_extension(x509.ExtendedKeyUsage([usage]), critical=False)
             .sign(ca_key, hashes.SHA256()))
        write(name, k, c)

    from cryptography.x509.oid import ExtendedKeyUsageOID
    leaf("server", "localhost", [x509.DNSName("localhost"), x509.DNSName("router.lab"),
                                 x509.IPAddress(ipaddress.ip_address("127.0.0.1"))],
         ExtendedKeyUsageOID.SERVER_AUTH)
    leaf("client", "yangscope", [x509.DNSName("yangscope")], ExtendedKeyUsageOID.CLIENT_AUTH)


def main():
    global pb, pb_grpc
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=57400)
    p.add_argument("--config", default=os.path.join(HERE, "config.json"))
    p.add_argument("--state", default=os.path.join(HERE, "state.json"))
    p.add_argument("--user", help="require this username (call metadata)")
    p.add_argument("--password", default="")
    p.add_argument("--tls", metavar="CERTDIR", help="serve TLS with certificates in CERTDIR")
    p.add_argument("--mtls", action="store_true", help="require a client certificate")
    p.add_argument("--models", nargs="*", default=[
        "ys-test@2026-10-24", "ys-test-aug@2026-10-24", "ietf-interfaces@2018-02-20",
        "ietf-ip@2018-02-22", "iana-if-type@2014-05-08"])
    p.add_argument("--tick", type=float, default=1.0, help="counter update interval (s)")
    args = p.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(name)s: %(message)s")
    pb, pb_grpc = load_stubs()

    with open(args.config) as f:
        config = json.load(f)
    with open(args.state) as f:
        state = json.load(f)
    store = Store(config, state)

    def ticker():
        while True:
            time.sleep(args.tick)
            store.tick()
    threading.Thread(target=ticker, daemon=True).start()

    service = GnmiService(args, store)
    handlers = {
        "Capabilities": grpc.unary_unary_rpc_method_handler(
            service.Capabilities, pb.CapabilityRequest.FromString, pb.CapabilityResponse.SerializeToString),
        "Get": grpc.unary_unary_rpc_method_handler(
            service.Get, pb.GetRequest.FromString, pb.GetResponse.SerializeToString),
        "Set": grpc.unary_unary_rpc_method_handler(
            service.Set, pb.SetRequest.FromString, pb.SetResponse.SerializeToString),
        "Subscribe": grpc.stream_stream_rpc_method_handler(
            service.Subscribe, pb.SubscribeRequest.FromString, pb.SubscribeResponse.SerializeToString),
    }
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler("gnmi.gNMI", handlers),))
    addr = f"{args.host}:{args.port}"
    if args.tls:
        make_certs(args.tls)
        rd = lambda n: open(os.path.join(args.tls, n), "rb").read()  # noqa: E731
        creds = grpc.ssl_server_credentials([(rd("server-key.pem"), rd("server.pem"))],
                                            root_certificates=rd("ca.pem") if args.mtls else None,
                                            require_client_auth=args.mtls)
        port = server.add_secure_port(addr, creds)
    else:
        port = server.add_insecure_port(addr)
    server.start()
    print(f"listening on {args.host}:{port}{' (TLS)' if args.tls else ''}", flush=True)
    server.wait_for_termination()


if __name__ == "__main__":
    main()
