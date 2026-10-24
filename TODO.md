# TODO

## Phase 1 – offline browser (polish)
- [ ] Jump from a node to its definition line in the Source tab
- [ ] Show where a node comes from: grouping/uses chain, augment, deviation
- [ ] Deviations view per module
- [ ] Per-module feature toggles instead of the global "enable all"
- [ ] Remember the loaded file set as a named "workspace" and reopen it
- [ ] Copy menu: JSON (RFC 7951) and XML instance skeleton for a subtree
- [ ] Dark-mode tuned badge colours
- [ ] Optional QtSvg icon instead of the embedded PNG

## Phase 2 – NETCONF
- [x] Device profiles (host, port, user, auth method; passwords not stored)
- [x] NETCONF over SSH on libssh (1.0 and 1.1 framing), hello/capabilities view
- [x] Download schemas from the device (hello, YANG library 1.0/1.1, RFC 6022 get-schema)
- [x] get / get-config with a subtree or XPath filter built from the selected node
- [x] Data tree view of replies, typed through libyang; XML and JSON views
- [ ] Store passwords in the OS keyring (opt-in)
- [x] Key values in filters: pick a list entry from the data tree to filter on
- [x] edit-config with local validation (schemas + current config) and a change preview
- [x] lock/unlock, validate, commit (with preview), confirmed commit, cancel-commit, discard-changes
- [ ] Edit values directly in the data tree (inline editors per type)
- [ ] Lock running as well (some devices want both locked for a commit)
- [ ] persist / persist-id for confirmed commits across sessions
- [ ] copy-config / delete-config, startup datastore
- [ ] NMDA get-data (RFC 8526) for operational and intended datastores
- [ ] Notifications (create-subscription) viewer
- [ ] Several devices at once (one panel per session)
- [ ] Call Home listener
- [ ] Jump host / ProxyJump support

## Phase 3 – gNMI
- [x] gRPC client (gnmi.proto), TLS / mutual TLS / server-name override, insecure mode for labs
- [x] Capabilities, Get, Set (validated locally with a change preview)
- [x] Subscribe (ONCE / POLL / STREAM, SAMPLE / ON_CHANGE / TARGET_DEFINED) with live table
- [x] Graphs of values and counter rates
- [x] Path completion from the loaded schema
- [ ] Test against real targets (SR Linux, cEOS, IOS XR, Junos) and fix quirks
- [ ] union_replace and gNMI extensions (history, commit confirmed, depth)
- [ ] Export the live table / graph data to CSV
- [ ] Several subscriptions side by side; alarms on thresholds
- [ ] origin handling for mixed CLI/OpenConfig targets

## Later
- [ ] Datastore and device-to-device diff
- [ ] RESTCONF client
- [ ] Packaging: Flatpak, RPM/DEB, winget
