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

Debug logs are written by injected target processes to:

```text
/data/adb/modules/Yukari/logs/yukari.log
```

## Current native behavior

- Zygisk app specialization matches configured target packages.
- Java `ServiceManager.sCache` entries containing fixed ROM keywords are removed.
- Binder request filtering rewrites matching service lookup names before they reach ServiceManager.
- Binder reply filtering rewrites matching `listServices` / service-manager reply names before the target app reads them.
- Binder interception uses Zygisk's standard PLT hook API instead of patching libc inline.

Reply filtering uses same-length placeholders instead of changing Parcel size. This removes Duck-style keyword hits while preserving Binder parcel layout.

## Configuration

```json
{
  "enabled": true,
  "targets": [
    "com.example.app"
  ]
}
```

## Build

```bash
gradle :module:assembleRelease :module:assembleDebug
bash scripts/package.sh release
bash scripts/package.sh debug
```
