# On-device acceptance checks

Host tests validate parsing, JNI failure handling and ownership logic. They do
not execute ART, Zygisk, or the Binder driver. No Android device result is implied
by a passing host test. Record device model, ROM build, API level, Zygisk provider
and module commit for every device run.

## Setup

1. Build/package the module and install the ZIP. Reboot after replacing the module;
   never overwrite a shared library that zygote still maps.
2. Add a probe application's package to `targets`. Use the same probe in an
   untargeted control application, or compare runs with `enabled: false`.
3. Begin with `hide_features` and `hide_resources` false, then enable each
   independently before testing both together. Force-stop and reopen the apps
   after configuration changes.
4. Run `bash scripts/diagnose_device.sh package.name [adb-serial]`. It reads
   configuration, process identities and Yukari logs without changing the device.
   A loading log proves installation, not successful behavior filtering.

## Acceptance matrix

| Channel | Target process | Untargeted control |
| --- | --- | --- |
| Service lookup | Hidden names fail lookup; normal Android services remain available. Exercise get/check/wait/isDeclared APIs present on the device. | Original results. |
| Service enumeration | listServices/getServiceDebugInfo expose no real hidden name; same-length placeholders are expected. | Original names. |
| Feature query | hasSystemFeature returns false for each known Lineage feature present in the control. | Original support flags. |
| Feature enumeration | Initial PackageManager reply exposes no real configured feature name when enumeration status is ready. | Original names. |
| Resource name lookup | getIdentifier with lineageos.platform returns 0. | Original resource ID when installed. |
| Resource ID access | Confirm the manager maps the ID's package to lineageos.platform; resource value/name access then behaves as absent. Unrelated resources, including unrelated package ID 0x3f, work. | Original values and names. |
| Private processes | A package target also applies to package:remote; an exact process target only applies to that process. | No injection. |
| System boundaries | System appIds, protected framework packages and system_server have no module instrumentation. | Normal system behavior. |

Service reflection/permission errors must be compared with the control; they are
not evidence of successful filtering. A feature/resource absent in the control
does not constitute a positive test. Resource arrays, themes and the assigned
package list are outside this resource channel. Native PackageManager calls and
deferred ParceledListSlice chunks are outside the feature channel. Unknown reply
headers or explicit Parcel rewinds conservatively leave feature names visible.

## Compatibility and failure checks

- Record each channel's ready/partial/unavailable log, not just process loading.
  Missing JNI signatures must disable only that capability and leave the app usable.
- Cover Android 8/9 legacy and current Android versions available to the tester;
  signature probing is not a substitute for those runs.
- Exercise app startup, configuration changes, multiple threads, private processes,
  nested Binder callbacks, repeated feature queries and Parcel recycling. Include
  cross-thread Parcel release: host tests do not prove ART/driver lifetime safety.
- Run normal app operations after each option is enabled. Compare clean queries,
  unrelated resource IDs, Binder failures and application exceptions with the control.
- Do not infer file-path, system-property, native scan, or kernel-side hiding from
  these checks. Those channels are not modified by Yukari.
