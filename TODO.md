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
- [ ] Device profiles (host, port, user; secrets in the OS keyring)
- [ ] Connect over SSH with libnetconf2, hello/capabilities view
- [ ] Download schemas from the device (RFC 6022 get-schema / YANG library)
- [ ] get / get-config with a subtree or XPath filter built by clicking nodes
- [ ] Data tree view of replies, validated and typed through libyang
- [ ] edit-config: edit values in the tree, validate locally, show the RPC, then send
- [ ] Notifications (create-subscription) viewer
- [ ] Call Home listener

## Phase 3 – gNMI
- [ ] gRPC/TLS client (gnmi.proto), certificates and insecure mode for labs
- [ ] Capabilities, Get, Set
- [ ] Subscribe (ONCE / POLL / STREAM, SAMPLE / ON_CHANGE) with live table
- [ ] Simple graphs of counter rates
- [ ] Path completion from the loaded schema

## Later
- [ ] Datastore and device-to-device diff
- [ ] RESTCONF client
- [ ] Packaging: Flatpak, RPM/DEB, winget
