# Yukari

Yukari is a target-scoped Zygisk module that hides custom-ROM ServiceManager
signals. Matching is ASCII case-insensitive for `lineage`, `crdroid`, `aospa`,
`pixelexperience`, `omnirom`, `protonaosp`, plus the exact service name
`profile`.

The preferred implementation hooks `android.os.BinderProxy.transactNative` via
Zygisk's JNI hook API. ServiceManager enumeration/debug replies are filtered at
the Parcel layer (direct `getService`/`checkService` lookups are left intact to
avoid startup null-Binder failures);
so libbinder PLT/GOT relocations are not modified. `listServices` and
`getServiceDebugInfo` are filtered without changing UTF-16 string lengths, and
`ServiceManager.sCache` is cleaned during app specialization. Systems without
the stable JNI entry point use the legacy ioctl filter as a fallback; its PLT
replacement points at an anonymous RX trampoline.

Private ELF symbols are hidden with a linker version script and stripped from
release artifacts. The module mapping can still be visible in `/proc/self/maps`
because the JNI callback must remain resident; unloading it safely would
require relocating the complete C++ runtime and is intentionally avoided.

See [README.zh-CN.md](README.zh-CN.md) for the detailed design and verification
commands.

## Configuration

```json
{
  "enabled": true,
  "force_denylist_unmount": true,
  "targets": ["com.example.app"]
}
```

Set `force_denylist_unmount` to `false` on devices where target apps depend
on Magisk-provided mounts; service filtering remains enabled.

Run `module/action.sh` (installed as `/data/adb/modules/Yukari/action.sh`) to
select targets. `a` merges all discovered third-party apps, `s` merges selected
numbers, `r` replaces targets, `k` preserves the current list, and `q` cancels.
When no terminal is available the script preserves the existing configuration.
Owner, secondary-user and work-profile package lists are merged.

## Build

```bash
./gradlew :module:assembleRelease
bash scripts/package.sh
```
