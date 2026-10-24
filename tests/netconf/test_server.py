#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal NETCONF-over-SSH server for testing YangScope.

Not a real device: running and candidate datastores live in memory, seeded
from --config; <get> adds the state data from --data. It supports get,
get-config, edit-config (merge/replace/create/delete/remove, default-operation,
test-only), lock/unlock, commit (also confirmed, with cancel-commit),
discard-changes, validate, get-schema and close-session, with enough of
RFC 6241 subtree filtering for the tests. List entries are matched on the
leaf children named in KEYS, which is enough for the test models. Both
NETCONF 1.0 (end-of-message) and 1.1 (chunked) framing are supported.

    python3 test_server.py --port 8830 --user admin --password admin \
        --schemas DIR [--schemas DIR2] --data data.xml --config config.xml [--base 1.0]
"""
import argparse
import copy
import logging
import os
import re
import socket
import sys
import threading
import xml.etree.ElementTree as ET

import paramiko

NC = "urn:ietf:params:xml:ns:netconf:base:1.0"
MON = "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring"
EOM = b"]]>]]>"
PREFIXES = {}  # prefix -> namespace of every scanned module
ARGS = None
log = logging.getLogger("nc-test-server")


# --- YANG module discovery ---------------------------------------------------

def scan_schemas(directory):
    """Return [(name, revision, namespace, path, is_submodule)] for .yang files."""
    out = []
    for fn in sorted(os.listdir(directory)):
        if not fn.endswith(".yang"):
            continue
        path = os.path.join(directory, fn)
        text = open(path, encoding="utf-8").read()
        text_nc = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
        m = re.search(r"^\s*(module|submodule)\s+([\w.\-]+)", text_nc)
        if not m:
            continue
        kind, name = m.groups()
        rev = re.search(r"\brevision\s+\"?(\d{4}-\d{2}-\d{2})", text_nc)
        ns = re.search(r"\bnamespace\s+\"([^\"]+)\"", text_nc)
        prefix = re.search(r"^\s*prefix\s+\"?([\w.\-]+)", text_nc, flags=re.M)
        if ns and prefix:
            # Serialize with the module's own prefixes, so prefixed values such
            # as identityrefs ("yst:udp") keep a matching xmlns declaration.
            ET.register_namespace(prefix.group(1), ns.group(1))
            PREFIXES[prefix.group(1)] = ns.group(1)
        out.append((name, rev.group(1) if rev else "", ns.group(1) if ns else "",
                    path, kind == "submodule"))
    return out


# --- subtree filtering (RFC 6241 section 6, simplified) -----------------------

def _matches(filt, node):
    return filt.tag == node.tag


def _apply(filt, node):
    """Return a filtered copy of node according to filter element filt, or None."""
    children = list(filt)
    if not children:
        text = (filt.text or "").strip()
        if text and (node.text or "").strip() != text:
            return None  # content match node
        return node  # selection node: whole subtree
    # content match nodes (leaf children with text) must all match
    content = [c for c in children if not list(c) and (c.text or "").strip()]
    for c in content:
        found = [n for n in node if n.tag == c.tag and (n.text or "").strip() == c.text.strip()]
        if not found:
            return None
    selection = [c for c in children if c not in content]
    copy = ET.Element(node.tag, node.attrib)
    copy.text = node.text
    if not selection:
        # only content matches: return the whole node
        for n in node:
            copy.append(n)
        return copy
    for n in node:
        if any(n.tag == c.tag for c in content):
            copy.append(n)
            continue
        for c in selection:
            if _matches(c, n):
                r = _apply(c, n)
                if r is not None:
                    copy.append(r)
    return copy


def subtree_filter(roots, filt):
    if filt is None:
        return roots
    out = []
    for f in filt:
        for r in roots:
            if _matches(f, r):
                res = _apply(f, r)
                if res is not None:
                    out.append(res)
    return out


def serialize(elem):
    """ElementTree drops namespace declarations that are only used inside
    text (identityref values such as "ianaift:ethernetCsmacd"); add them back
    on the root element like a real server keeps them."""
    text = ET.tostring(elem, encoding="unicode")
    end = text.find(">")
    if text[end - 1] == "/":
        end -= 1
    extra = ""
    for prefix, ns in PREFIXES.items():
        if f">{prefix}:" in text and f"xmlns:{prefix}=" not in text:
            extra += f' xmlns:{prefix}="{ns}"'
    return text[:end] + extra + text[end:]


# --- datastores ----------------------------------------------------------------

KEYS = ("name", "port", "ip")


def local(tag):
    return tag.split("}", 1)[1] if "}" in tag else tag


def identity(elem):
    keys = tuple((local(c.tag), (c.text or "").strip()) for c in elem
                 if local(c.tag) in KEYS and not len(c))
    return elem.tag, keys


def find_match(parent_children, elem):
    if not len(elem):
        leaves = [c for c in parent_children if c.tag == elem.tag and not len(c)]
        if len(leaves) > 1:   # leaf-list: entries are identified by value
            text = (elem.text or "").strip()
            return next((c for c in leaves if (c.text or "").strip() == text), None)
        return leaves[0] if leaves else None
    ident = identity(elem)
    return next((c for c in parent_children if len(c) and identity(c) == ident), None)


class EditError(Exception):
    def __init__(self, tag, msg):
        super().__init__(msg)
        self.tag = tag


def strip_ops(elem):
    e = copy.deepcopy(elem)
    for x in e.iter():
        x.attrib.pop(f"{{{NC}}}operation", None)
    return e


def apply_edit(children, edit, inherited):
    """Apply one edit element to a list of sibling elements (in place)."""
    op = edit.get(f"{{{NC}}}operation") or inherited
    match = find_match(children, edit)
    if op == "delete":
        if match is None:
            raise EditError("data-missing", f"{local(edit.tag)} does not exist")
        children.remove(match)
        return
    if op == "remove":
        if match is not None:
            children.remove(match)
        return
    if op == "create":
        if match is not None:
            raise EditError("data-exists", f"{local(edit.tag)} already exists")
        children.append(strip_ops(edit))
        return
    if op == "replace":
        if match is not None:
            children.insert(children.index(match), strip_ops(edit))
            children.remove(match)
        else:
            children.append(strip_ops(edit))
        return
    # merge / none
    if not len(edit):
        if op == "merge":
            if match is not None and not len(match):
                match.text = edit.text
            else:
                children.append(strip_ops(edit))
        return
    if match is None:
        match = ET.Element(edit.tag, {k: v for k, v in edit.attrib.items()
                                      if k != f"{{{NC}}}operation"})
        children.append(match)
    kids = list(match)
    for c in edit:
        apply_edit(kids, c, op)
    for c in list(match):
        match.remove(c)
    match.extend(kids)


class Store:
    """Datastores shared by all sessions of this server process."""

    def __init__(self, config_path, data_path):
        self.mutex = threading.RLock()
        self.running = list(ET.parse(config_path).getroot()) if config_path else []
        self.candidate = copy.deepcopy(self.running)
        self.state = list(ET.parse(data_path).getroot()) if data_path else []
        self.locks = {}            # datastore -> session id
        self.confirm_backup = None  # running before a confirmed commit
        self.confirm_timer = None

    def datastore(self, name):
        if name == "running":
            return self.running
        if name == "candidate":
            return self.candidate
        raise EditError("invalid-value", f"datastore {name} not supported")

    def set_datastore(self, name, roots):
        if name == "running":
            self.running = roots
        else:
            self.candidate = roots

    def operational(self):
        """running config plus the state-only parts of --data"""
        out = copy.deepcopy(self.running)

        def overlay(dst, src):
            for s in src:
                m = find_match(dst, s) if len(s) else next(
                    (d for d in dst if d.tag == s.tag and not len(d)), None)
                if m is None:
                    if not config_has(s):
                        dst.append(copy.deepcopy(s))
                elif len(s):
                    kids = list(m)
                    overlay(kids, list(s))
                    for c in list(m):
                        m.remove(c)
                    m.extend(kids)
        initial = list(ET.parse(ARGS.config).getroot()) if ARGS.config else []

        def config_has(elem):
            # True if this element is configuration (so a missing one was deleted)
            return elem.tag in {e.tag for r in initial for e in r.iter()}
        overlay(out, self.state)
        return out

    def check_lock(self, ds, session_id):
        owner = self.locks.get(ds)
        if owner is not None and owner != session_id:
            raise EditError("in-use", f"{ds} is locked by session {owner}")

    def release(self, session_id):
        for ds, owner in list(self.locks.items()):
            if owner == session_id:
                del self.locks[ds]
                if ds == "candidate":
                    # RFC 6241 8.3.5.2: pending changes are discarded
                    self.candidate = copy.deepcopy(self.running)

    def start_confirm(self, timeout):
        self.cancel_timer()
        self.confirm_timer = threading.Timer(timeout, self.rollback_confirmed)
        self.confirm_timer.daemon = True
        self.confirm_timer.start()

    def cancel_timer(self):
        if self.confirm_timer is not None:
            self.confirm_timer.cancel()
            self.confirm_timer = None

    def rollback_confirmed(self):
        with self.mutex:
            if self.confirm_backup is not None:
                log.info("confirmed commit timed out, rolling back")
                self.running = self.confirm_backup
                self.candidate = copy.deepcopy(self.running)
                self.confirm_backup = None
            self.confirm_timer = None


def normalize_prefixes(elem, nsmap):
    """Rewrite prefixed values ("x:tcp") to the module prefix this server
    serializes with, so identityrefs survive ElementTree."""
    reverse = {ns: p for p, ns in PREFIXES.items()}
    for e in elem.iter():
        t = (e.text or "").strip()
        m = re.match(r"^([A-Za-z_][\w.\-]*):(\S+)$", t)
        if m and m.group(1) in nsmap and nsmap[m.group(1)] in reverse:
            e.text = f"{reverse[nsmap[m.group(1)]]}:{m.group(2)}"


# --- SSH plumbing ------------------------------------------------------------

class Server(paramiko.ServerInterface):
    def __init__(self, user, password):
        self.user, self.password = user, password
        self.event = threading.Event()

    def check_auth_password(self, username, password):
        ok = username == self.user and password == self.password
        return paramiko.AUTH_SUCCESSFUL if ok else paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else \
            paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_subsystem_request(self, channel, name):
        if name == "netconf":
            self.event.set()
            return True
        return False


class Session:
    def __init__(self, chan, args, session_id):
        self.chan, self.args, self.session_id = chan, args, session_id
        self.buf = b""
        self.chunked = False
        self.nsmap = {}
        self.schemas = [s for d in args.schemas for s in scan_schemas(d)]

    # framing
    def send(self, xml):
        data = xml.encode() if isinstance(xml, str) else xml
        if self.chunked:
            self.chan.sendall(b"\n#%d\n" % len(data) + data + b"\n##\n")
        else:
            self.chan.sendall(data + EOM)

    def recv_msg(self):
        while True:
            if self.chunked:
                msg = self._try_chunked()
            else:
                i = self.buf.find(EOM)
                msg = None
                if i >= 0:
                    msg, self.buf = self.buf[:i], self.buf[i + len(EOM):]
            if msg is not None:
                return msg
            data = self.chan.recv(65536)
            if not data:
                return None
            self.buf += data

    def _try_chunked(self):
        pos, out = 0, b""
        while True:
            m = re.match(rb"\n#(\d+)\n", self.buf[pos:])
            if m:
                size = int(m.group(1))
                start = pos + m.end()
                if len(self.buf) < start + size:
                    return None
                out += self.buf[start:start + size]
                pos = start + size
                continue
            if self.buf[pos:pos + 4] == b"\n##\n":
                self.buf = self.buf[pos + 4:]
                return out
            return None

    # protocol
    def hello(self):
        caps = ["urn:ietf:params:netconf:base:1.0"]
        if self.args.base == "1.1":
            caps.append("urn:ietf:params:netconf:base:1.1")
        caps.append("urn:ietf:params:netconf:capability:xpath:1.0")
        caps.append("urn:ietf:params:netconf:capability:candidate:1.0")
        caps.append("urn:ietf:params:netconf:capability:confirmed-commit:1.1")
        caps.append("urn:ietf:params:netconf:capability:validate:1.1")
        caps.append("urn:ietf:params:netconf:capability:rollback-on-error:1.0")
        if self.args.writable_running:
            caps.append("urn:ietf:params:netconf:capability:writable-running:1.0")
        for name, rev, ns, _, sub in self.schemas:
            if sub or not ns:
                continue
            cap = f"{ns}?module={name}"
            if rev:
                cap += f"&amp;revision={rev}"
            if name in self.args.features_for:
                cap += f"&amp;features={self.args.features_for[name]}"
            caps.append(cap)
        body = "".join(f"<capability>{c}</capability>" for c in caps)
        self.send(f'<?xml version="1.0" encoding="UTF-8"?><hello xmlns="{NC}">'
                  f"<capabilities>{body}</capabilities>"
                  f"<session-id>{self.session_id}</session-id></hello>")
        msg = self.recv_msg()
        if msg is None:
            return False
        client = ET.fromstring(msg)
        client_caps = [c.text for c in client.iter(f"{{{NC}}}capability")]
        self.chunked = (self.args.base == "1.1"
                        and "urn:ietf:params:netconf:base:1.1" in client_caps)
        log.info("client hello, chunked=%s", self.chunked)
        return True

    def reply(self, rpc, inner):
        mid = rpc.get("message-id", "")
        attrs = "".join(f' {k}="{v}"' for k, v in rpc.attrib.items() if k != "message-id")
        self.send(f'<rpc-reply xmlns="{NC}" message-id="{mid}"{attrs}>{inner}</rpc-reply>')

    def error(self, rpc, tag, msg):
        self.reply(rpc, f"<rpc-error><error-type>application</error-type>"
                        f"<error-tag>{tag}</error-tag><error-severity>error</error-severity>"
                        f"<error-message>{msg}</error-message></rpc-error>")

    def load_roots(self, path):
        if not path:
            return []
        root = ET.parse(path).getroot()  # wrapper element, children are top-level nodes
        return list(root)

    def monitoring_schemas(self):
        items = []
        for name, rev, ns, _, _sub in self.schemas:
            items.append(
                f"<schema><identifier>{name}</identifier><version>{rev}</version>"
                f"<format>yang</format><namespace>{ns}</namespace>"
                f"<location>NETCONF</location></schema>")
        return ET.fromstring(f'<netconf-state xmlns="{MON}"><schemas>{"".join(items)}'
                             f"</schemas></netconf-state>")

    def handle(self, rpc):
        op = list(rpc)[0] if len(rpc) else None
        if op is None:
            return self.error(rpc, "missing-element", "empty rpc")
        tag = op.tag
        if tag == f"{{{NC}}}close-session":
            self.reply(rpc, "<ok/>")
            return False
        store = self.args.store
        name = local(tag)
        if tag.startswith(f"{{{NC}}}") and name in ("edit-config", "lock", "unlock", "commit",
                                                    "discard-changes", "validate", "cancel-commit"):
            try:
                with store.mutex:
                    return self.handle_config_op(rpc, op, name, store)
            except EditError as e:
                return self.error(rpc, e.tag, str(e))
        if tag in (f"{{{NC}}}get", f"{{{NC}}}get-config"):
            if tag.endswith("get-config"):
                ds = op.find(f"{{{NC}}}source")
                ds_name = local(list(ds)[0].tag) if ds is not None and len(ds) else "?"
                try:
                    with store.mutex:
                        roots = copy.deepcopy(store.datastore(ds_name))
                except EditError as e:
                    return self.error(rpc, e.tag, str(e))
            else:
                with store.mutex:
                    roots = store.operational() + [self.monitoring_schemas()]
            filt = op.find(f"{{{NC}}}filter")
            if filt is not None and filt.get("type") == "xpath":
                return self.error(rpc, "operation-not-supported", "xpath filter not implemented in test server")
            res = subtree_filter(roots, filt)
            body = "".join(serialize(r) for r in res)
            return self.reply(rpc, f"<data>{body}</data>")
        if tag == f"{{{MON}}}get-schema":
            ident = op.findtext(f"{{{MON}}}identifier")
            version = op.findtext(f"{{{MON}}}version") or ""
            for name, rev, _, path, _ in self.schemas:
                if name == ident and (not version or version == rev):
                    text = open(path, encoding="utf-8").read()
                    esc = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                    return self.reply(rpc, f'<data xmlns="{MON}">{esc}</data>')
            return self.error(rpc, "invalid-value", f"schema {ident} not found")
        return self.error(rpc, "operation-not-supported", f"{tag} not supported")

    def handle_config_op(self, rpc, op, name, store):
        ds_of = lambda el: local(list(el)[0].tag) if el is not None and len(el) else None  # noqa: E731
        if name == "lock":
            ds = ds_of(op.find(f"{{{NC}}}target"))
            store.datastore(ds)
            owner = store.locks.get(ds)
            if owner is not None:
                raise EditError("lock-denied", f"{ds} is already locked by session {owner}")
            store.locks[ds] = self.session_id
            return self.reply(rpc, "<ok/>")
        if name == "unlock":
            ds = ds_of(op.find(f"{{{NC}}}target"))
            if store.locks.get(ds) != self.session_id:
                raise EditError("operation-failed", f"{ds} is not locked by this session")
            del store.locks[ds]
            return self.reply(rpc, "<ok/>")
        if name == "edit-config":
            ds = ds_of(op.find(f"{{{NC}}}target"))
            if ds == "running" and not self.args.writable_running:
                raise EditError("operation-not-supported", "running is not writable; edit candidate")
            store.check_lock(ds, self.session_id)
            default = op.findtext(f"{{{NC}}}default-operation") or "merge"
            test = op.findtext(f"{{{NC}}}test-option") or "test-then-set"
            config = op.find(f"{{{NC}}}config")
            if config is None:
                raise EditError("missing-element", "edit-config without config")
            normalize_prefixes(config, self.nsmap)
            work = [] if default == "replace" else copy.deepcopy(store.datastore(ds))
            for e in config:
                apply_edit(work, e, "merge" if default == "replace" else default)
            if test != "test-only":
                store.set_datastore(ds, work)
            return self.reply(rpc, "<ok/>")
        if name == "discard-changes":
            store.candidate = copy.deepcopy(store.running)
            return self.reply(rpc, "<ok/>")
        if name == "validate":
            return self.reply(rpc, "<ok/>")
        if name == "commit":
            store.check_lock("running", self.session_id)
            confirmed = op.find(f"{{{NC}}}confirmed") is not None
            if confirmed:
                if store.confirm_backup is None:
                    store.confirm_backup = copy.deepcopy(store.running)
                timeout = int(op.findtext(f"{{{NC}}}confirm-timeout") or "600")
                store.start_confirm(timeout)
            else:
                store.cancel_timer()
                store.confirm_backup = None
            store.running = copy.deepcopy(store.candidate)
            return self.reply(rpc, "<ok/>")
        if name == "cancel-commit":
            if store.confirm_backup is None:
                raise EditError("operation-failed", "no confirmed commit is pending")
            store.cancel_timer()
            store.running = store.confirm_backup
            store.candidate = copy.deepcopy(store.running)
            store.confirm_backup = None
            return self.reply(rpc, "<ok/>")
        raise EditError("operation-not-supported", name)

    def run(self):
        try:
            self._run()
        finally:
            with self.args.store.mutex:
                self.args.store.release(self.session_id)

    def _run(self):
        if not self.hello():
            return
        while True:
            msg = self.recv_msg()
            if msg is None:
                return
            try:
                # remember prefix declarations for prefixed values (identityrefs)
                self.nsmap = dict(re.findall(rb'xmlns:([A-Za-z_][\w.\-]*)="([^"]*)"', msg))
                self.nsmap = {k.decode(): v.decode() for k, v in self.nsmap.items()}
                rpc = ET.fromstring(msg)
            except ET.ParseError as e:
                log.warning("bad xml: %s", e)
                continue
            if self.handle(rpc) is False:
                return


def load_host_key(path):
    """Use a fixed host key (created on first use) or a throw-away one."""
    if not path:
        return paramiko.RSAKey.generate(2048)
    if not os.path.exists(path):
        paramiko.RSAKey.generate(2048).write_private_key_file(path)
    return paramiko.RSAKey(filename=path)


def serve(args):
    host_key = load_host_key(args.host_key)
    if args.known_hosts_line:
        print(f"[{args.host}]:{args.port} {host_key.get_name()} {host_key.get_base64()}")
        return
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.listen(5)
    # --port 0 picks a free port; report the real one for the test harness
    print(f"listening on {args.host}:{sock.getsockname()[1]}", flush=True)
    sid = 0
    while True:
        client, _ = sock.accept()
        sid += 1
        threading.Thread(target=handle_client, args=(client, host_key, args, sid),
                         daemon=True).start()
        if args.once:
            break
    while args.once and threading.active_count() > 1:
        threading.Event().wait(0.1)


def handle_client(client, host_key, args, sid):
    t = paramiko.Transport(client)
    t.add_server_key(host_key)
    server = Server(args.user, args.password)
    try:
        t.start_server(server=server)
        chan = t.accept(20)
        if chan is None or not server.event.wait(10):
            return
        Session(chan, args, sid).run()
        chan.close()
    except Exception as e:  # noqa: BLE001 - test helper, log and drop
        log.warning("session %d ended: %s", sid, e)
    finally:
        t.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8830)
    p.add_argument("--user", default="admin")
    p.add_argument("--password", default="admin")
    p.add_argument("--schemas", action="append", required=True,
                   help="directory with .yang files (repeatable)")
    p.add_argument("--data", help="XML file: wrapper element around config+state roots")
    p.add_argument("--config", help="XML file: wrapper element around config roots")
    p.add_argument("--base", choices=["1.0", "1.1"], default="1.1")
    p.add_argument("--feature", action="append", default=[],
                   help="module=feat1,feat2 to advertise in the capability")
    p.add_argument("--once", action="store_true", help="exit after one session")
    p.add_argument("--host-key", help="RSA host key file (created if missing)")
    p.add_argument("--writable-running", action="store_true",
                   help="advertise :writable-running and accept edits to running")
    p.add_argument("--known-hosts-line", action="store_true",
                   help="print the known_hosts entry for --host-key/--port and exit")
    args = p.parse_args()
    args.features_for = dict(f.split("=", 1) for f in args.feature)
    global ARGS
    ARGS = args
    for d in args.schemas:   # register module prefixes before parsing the stores
        scan_schemas(d)
    args.store = Store(args.config, args.data)
    logging.basicConfig(level=logging.INFO, format="%(name)s: %(message)s")
    serve(args)


if __name__ == "__main__":
    sys.exit(main())
