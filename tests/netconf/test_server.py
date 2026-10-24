#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal NETCONF-over-SSH server for testing YangScope.

Not a real datastore: it serves fixed XML documents, supports the RPCs
YangScope uses (get, get-config, get-schema, close-session) and implements
enough of RFC 6241 subtree filtering for the tests. Both NETCONF 1.0
(end-of-message) and 1.1 (chunked) framing are supported.

    python3 test_server.py --port 8830 --user admin --password admin \
        --schemas DIR [--schemas DIR2] --data data.xml --config config.xml [--base 1.0]
"""
import argparse
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
        if tag in (f"{{{NC}}}get", f"{{{NC}}}get-config"):
            if tag.endswith("get-config"):
                ds = op.find(f"{{{NC}}}source")
                ds_name = list(ds)[0].tag.split("}")[1] if ds is not None and len(ds) else "?"
                if ds_name != "running":
                    return self.error(rpc, "invalid-value", f"datastore {ds_name} not supported")
                roots = self.load_roots(self.args.config)
            else:
                roots = self.load_roots(self.args.data) + [self.monitoring_schemas()]
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

    def run(self):
        if not self.hello():
            return
        while True:
            msg = self.recv_msg()
            if msg is None:
                return
            try:
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
    p.add_argument("--known-hosts-line", action="store_true",
                   help="print the known_hosts entry for --host-key/--port and exit")
    args = p.parse_args()
    args.features_for = dict(f.split("=", 1) for f in args.feature)
    logging.basicConfig(level=logging.INFO, format="%(name)s: %(message)s")
    serve(args)


if __name__ == "__main__":
    sys.exit(main())
