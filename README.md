# Yukari

Zygisk module that hides selected custom-ROM service signals from configured target applications.

## Module identity

- Name: `Yukari`
- Module ID: `Yukari`
- Configuration: `/data/adb/modules/Yukari/config.json`
- Runtime: standard Zygisk-compatible loader, tested target is Zygisk Next on `arm64-v8a`

## Fixed service keywords

Matching is ASCII case-insensitive and built into the module:

- `lineage`
- `crdroid`
- `aospa`
- `pixelexperience`
- `omnirom`
- `protonaosp`

Users configure target applications only. The module does not stop or unregister services globally; filtering is scoped to selected application processes.

## Artifacts

CI produces two flashable module zips:

- `Yukari.zip`: normal build
- `Yukari-debug.zip`: debug build with file logging enabled

Debug logs are written to logcat with tag `Yukari`.

## Current native behavior

- Zygisk app specialization matches configured target packages.
- Java `ServiceManager.sCache` entries containing fixed ROM keywords are removed.
- Binder request filtering rewrites matching service lookup names before they reach ServiceManager.
- Binder reply filtering is attempted only when the reply buffer is already writable.
- Optional enhanced mode can filter read-only Binder replies by swapping the reply buffer pointer to a modified userspace copy. This can hide `listServices` results, but it is unsafe because Binder buffer ownership no longer matches the pointer libbinder sees.
- Binder interception uses Zygisk's standard PLT hook API instead of patching libc inline.

Request/reply filtering uses same-length placeholders instead of changing Parcel size.

## Configuration

```json
{
  "enabled": true,
  "enhancedMode": false,
  "targets": [
    "com.example.app"
  ]
}
```

Set `enhancedMode` to `true` only for testing if you accept the Binder buffer ownership risk.

## Build

```bash
gradle :module:assembleRelease :module:assembleDebug
bash scripts/package.sh release
bash scripts/package.sh debug
```
