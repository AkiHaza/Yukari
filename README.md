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
- Binder request filtering verifies the ServiceManager interface and rewrites
  only the first service-name argument in a private copy. Original request
  bytes and write commands remain unchanged, including partial-write retries.
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
  "hide_resources": false,
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

`hide_resources` independently hides `lineageos.platform` resource identifiers,
single values and name lookups. Resource IDs are filtered only after the current
AssetManager confirms their package mapping; an unrelated `0x3f` package remains
untouched. Missing mappings leave ID access visible. Arrays, themes and package
map enumeration are outside this channel. This option also defaults to false and
is preserved by `action.sh`.

### Build

```bash
bash scripts/build.sh test     # Host tests; no Android SDK required
bash scripts/build.sh android  # SDK preparation, Gradle release, verified ZIP
# Or run both: bash scripts/build.sh all
```

Android builds require JDK 17 and an Android SDK with command-line tools. Set
`JAVA_HOME` and `ANDROID_HOME` (or `ANDROID_SDK_ROOT`). The official checked
Gradle Wrapper supplies Gradle 8.11.1; SDK/NDK/CMake versions are shared in
`gradle.properties`. Output: `out/Yukari.zip`. See [build details](docs/build.md).

CI runs host validation separately from Android packaging. Every run logs the
checked-out commit and retains phase logs under `out/diagnostics/`. Push the new
commit to run the new workflow; rerunning an old Actions run keeps its old commit.

Native regression tests run on Linux/WSL with a C++20 compiler, CMake, make,
binutils and JDK JNI headers; see [test prerequisites](tests/native/README.md):

```bash
bash tests/native/run.sh
```

Channels install independently before specialization. After specialization,
Yukari reports `services`, `cache`, `feature_query`, `feature_enum` and `resources`
as `off`, `ready`, `partial` (resources only), or `unavailable`. These are setup
states, not proof that a device check passed. Cache failure does not disable
the other installed channels.

For read-only loading diagnostics run
`bash scripts/diagnose_device.sh package.name [adb-serial]`. Follow the
[device acceptance checklist](tests/device/README.md) to verify real API behavior.
