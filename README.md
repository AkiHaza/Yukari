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

Users configure target applications only. The module does not stop or unregister services globally; filtering is scoped to selected application processes.

### Current native behavior

- Zygisk app specialization matches configured target packages.
- Java `ServiceManager.sCache` entries containing fixed ROM keywords are removed.
- Binder request filtering rewrites matching service lookup names before they reach ServiceManager.
- Binder reply filtering uses a safe buffer swap mechanism to scan and filter all ServiceManager replies (including `listServices` and `getServiceDebugInfo`). This prevents apps from enumerating hidden services.
- Binder interception uses Zygisk's standard PLT hook API instead of patching libc inline.

Request/reply filtering uses same-length placeholders instead of changing Parcel size.

### Configuration

```json
{
  "enabled": true,
  "targets": [
    "com.example.app"
  ]
}
```

### Build

```bash
gradle :module:assembleRelease
bash scripts/package.sh
```
