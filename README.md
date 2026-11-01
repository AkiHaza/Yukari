# Yukari

[English](README.md) | [中文](README.zh-CN.md)

---

Zygisk module that hides selected custom-ROM service signals from configured target applications.

### Fixed service keywords

Matching is ASCII case-insensitive and built into the module:

- `lineage`
- `crdroid`
- `aospa`
- `pixelexperience`
- `omnirom`
- `protonaosp`
- `profile`

Users configure target applications and optional filtering channels. The module does not stop or unregister services globally; filtering is scoped to selected application processes.

### Current native behavior

- Zygisk app specialization matches configured target packages.
- A package target also covers private processes such as `com.example.app:remote`;
  an explicit process target remains an exact match. System app IDs and protected
  framework packages are excluded, including their private processes.
- Java `ServiceManager.sCache` entries containing fixed ROM keywords are removed.
- Binder request filtering rewrites matching service lookup names before they reach ServiceManager.
- Binder reply filtering uses a safe buffer swap mechanism to scan and filter all ServiceManager replies (including `listServices` and `getServiceDebugInfo`). This prevents apps from enumerating hidden services.
- Binder interception uses Zygisk's standard PLT hook API instead of patching libc inline.

Request/reply filtering uses same-length placeholders instead of changing Parcel size.

Service names are matched in place without temporary strings. Replies without
hidden services retain their original Binder buffers; matching replies use a
reusable thread-local buffer that grows on demand, up to 256 KiB. Releasing a
filtered reply also returns its original buffer to the Binder driver.

### Configuration

```json
{
  "enabled": true,
  "hide_features": false,
  "targets": [
    "com.example.app"
  ]
}
```

`hide_features` opts selected processes into exact Lineage feature filtering.
Queries use a private Parcel; enumeration replaces returned strings only for a
tracked PackageManager feature reply. Query and enumeration support are reported
separately. Missing JNI methods or unsupported lifecycle ABIs leave the affected
capability visible. This covers Java calls and the initial enumeration reply;
deferred ParceledListSlice chunks, unknown reply headers, and explicit Parcel
repositioning are conservatively left visible. `action.sh` preserves this option.

### Build


```bash
gradle :module:assembleRelease
bash scripts/package.sh
```

Native regression tests can run on Linux with a C++20 compiler:

```bash
bash tests/native/run.sh
```
