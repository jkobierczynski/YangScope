# Development notes

## 2026-10-24 – v0.2.0, phase 2 (NETCONF client)

### Design
- **Own NETCONF client on libssh instead of libnetconf2.** libnetconf2 is
  POSIX-only, while YangScope targets Windows and macOS too. The protocol part
  is small: hello exchange, RFC 6242 framing (end-of-message for 1.0, chunked
  for 1.1, selected when both sides advertise base:1.1) and message-id
  matching. libssh is available everywhere (apt, Homebrew, vcpkg).
- **Layers:** `NetconfFraming` (pure, unit-tested byte-by-byte),
  `NetconfXml` (QXmlStreamReader/Writer: hello, rpc-reply, rpc-error,
  capability URIs, operation builders), `NetconfSession` (blocking SSH session,
  host key check, authentication, timeouts, cancel), `SchemaSync` (module list
  and schema download), `DataTree` (reply data parsed by libyang).
- **Threading:** the GUI never touches libssh. `NetconfWorker` lives on its own
  QThread; the host-key question is asked through a blocking queued call to a
  GUI-thread object.
- **Host keys:** known_hosts via libssh; unknown keys need an explicit
  "Trust once" or "Trust and remember", changed keys are refused outright.
- **Reply data keeps namespace declarations from rpc-reply/data ancestors**, so
  identityref values like `ianaift:ethernetCsmacd` still resolve when devices
  declare prefixes at the top of the reply.
- **Device schemas:** module list from YANG library 1.1, then 1.0, then hello
  capabilities. Files are named from device-supplied identifiers, so names are
  sanitised (`../../etc/passwd` becomes `_.._etc_passwd`). Modules are loaded
  with exactly the advertised features, which matters: e.g. `if-index` only
  exists with ietf-interfaces' `if-mib` feature.
- **Data parsing** uses `LYD_PARSE_ONLY | LYD_PARSE_OPAQ`: nothing is rejected,
  and elements the schemas don't describe show up flagged instead of vanishing.
- Anything holding libyang data (the reply tree) is released before the schema
  context is reset, and re-parsed afterwards.

### libyang differences found this round
| Area | Old | New | How |
|---|---|---|---|
| `lyd_node::parent` | `lyd_node_inner *` | `lyd_node *` | use `lyd_parent()` |
| `LYD_PRINT_WITHSIBLINGS` | name | renamed `LYD_PRINT_SIBLINGS` | value `0x01` in both |
| identityref in JSON | always `module:identity` | prefix dropped for the leaf's own module | tests accept both |

### Test server
`tests/netconf/test_server.py` (paramiko) serves fixed XML documents with
subtree filtering, `get-schema`, `ietf-netconf-monitoring`, both framings,
password auth, fixed or throw-away host keys and advertised features. The
tests start it on a free port; they never write to the real known_hosts.

### Verified
- `test_netconf`: 134 checks (framing, XML, live sessions over 1.1 and 1.0,
  rpc-error, schema download and cache, wrong password, refused host key)
  pass on libyang 2.1.30 and 6.4.13.
- GUI end to end under Xvfb: connect, load device schemas (6 modules), select
  `/ietf-interfaces:interfaces/interface`, get with subtree filter, typed data
  tree with no opaque nodes.

### Not yet verified
- Real devices (Junos, IOS XR, IOS XE, SR Linux, Netopeer2). Expect quirks in
  keyboard-interactive prompts and YANG library replies.
- Windows/macOS builds of the libssh part (CI only).

## 2026-10-24 – v0.1.0, phase 1 (offline schema browser)

### Design
- **Core / GUI split.** `src/core` wraps libyang and turns compiled schema nodes
  into names, paths and HTML details. It depends only on QtCore and is covered
  by `tests/test_core.cpp`. `src/gui` holds the widgets.
- **Compiled schema with parsed back-pointers.** The context is created with
  `LY_CTX_SET_PRIV_PARSED`, so every compiled node's `priv` points at its parsed
  node. That is where the type name as written (`oc-yang:counter64`), the
  default value and `if-feature` expressions live; resolved ranges, patterns,
  enums and identities come from the compiled type.
- **`LY_CTX_REF_IMPLEMENTED`.** OpenConfig uses identityref defaults from
  imported modules; without this flag 24 of the OpenConfig modules fail to
  compile ("identity found in non-implemented module").
- **Lazy tree model.** Children are created the first time a row is expanded,
  so the full OpenConfig set opens instantly.
- **Submodules** are detected by their first statement and skipped when a
  folder is opened; they are loaded through the main module's `include`.
- libyang's internal modules (ietf-yang-library, yang, ...) are hidden unless
  *Options → Show libyang Internal Modules* is on.

### libyang version compatibility
Tested against libyang 2.1.30 (Ubuntu 24.04) and 6.4.13 (latest tag at the
time). Differences handled:

| Area | Old | New | How |
|---|---|---|---|
| Log callback | `(level, msg, path)` + `ly_set_log_clb(cb, path)` | `(level, msg, data_path, schema_path, line)` | CMake `check_cxx_source_compiles` → `YANGSCOPE_LY_LOG_CLB_V3` |
| Sized arrays | `LY_ARRAY_COUNT/FOR` | `LYA_COUNT/FOR` | own `lyCount()` in `LyCompat.h` (both store a `uint64_t` count before the array) |
| Internal modules | embedded | loaded from `ly_yang_module_dir()` | context creation retries with bundled dirs and `ly_yang_module_dir()` |
| `ly_data_type2str` | not exported in every build | — | own type-name table |
| Verbose messages | filtered by `ly_log_level` | arrive at the callback | callback drops anything below warning |

### Verified
- Core tests: 49 checks pass on both libyang versions.
- Full OpenConfig release set (219 files, 42 submodules): 177 modules load
  with no failures in about 5 s; all 15,795 schema nodes render (same count as
  libyang's own `lysc_module_dfs_full`).
- IETF RFC set from YangModels: loads except modules whose IEEE/IANA imports
  were not present in the checkout.
- GUI run headless under Xvfb with screenshots (`YANGSCOPE_SCREENSHOT=file.png`).

### Not yet verified
- The GitHub Actions release workflow (Windows/vcpkg and macOS/Homebrew
  builds, AppImage) has not run yet; expect small fixes on the first tag.
