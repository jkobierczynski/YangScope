# Development notes

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
