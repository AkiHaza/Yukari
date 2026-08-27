# Yukari 启动失败风险排查报告

本报告基于 `Dev` 分支当前源码（`module/src/main/cpp/`、模块脚本和构建脚本）
整理。结论按“安全 / 有隐患”给出；能在不改变模块目标范围和过滤功能的地方已直接
修复。

## 结论摘要

| 检查项 | 结论 | 处理 |
| --- | --- | --- |
| Binder 直接 lookup 误伤 | 安全（已修复） | JNI/ioctl 路径均不改写 `getService`/`checkService` 请求 |
| `sCache` 清理时机 | 基本安全 | 仅在启动后清理，避免事务回调并发修改 `ArrayMap`；JNI 异常均自动清除 |
| `FORCE_DENYLIST_UNMOUNT` | 已缓解（仍需设备验证） | 仅目标进程启用；新增 `force_denylist_unmount` 配置开关，默认 `true`，兼容性问题可关闭 |
| 等长 String16 替换 | 安全 | 所有 offset/长度/对齐均做边界检查；过大或 malformed Parcel 跳过 |
| ioctl 回复缓冲释放 | 已修复 | `BC_FREE_BUFFER` 转发原始驱动指针，避免每次过滤泄漏 Binder 映射 |
| JNI Parcel 回复重写 | 安全（防御式） | 保留 `readException` 头、数组计数和 `ServiceDebugInfo.debugPid` |
| protected package | 已加强 | 增加权限、Provider、电话、蓝牙、NFC、输入法等启动关键组件 |
| 非目标生命周期 | 安全 | 非目标应用和 `system_server` 调用 `DLCLOSE_MODULE_LIBRARY`；目标不卸载已安装 Hook |
| 模块脚本/配置原子性 | 已修复 | 同目录临时文件、校验、chmod 后原子 `mv`，失败保留旧配置 |

## 1. Binder 请求过滤误伤

旧实现会在 native Binder 写入路径中尝试改写 `getService`/`checkService` 名称。对
`profile` 等服务返回空 Binder 会让框架初始化代码收到 null，典型结果是
`ClientTransactionListenerController` 或 ROM 自定义初始化逻辑 NPE。

当前实现的 JNI 主路径只用请求 Parcel 的 interface token 识别
`android.os.IServiceManager`，兼容 0/4/8/12/16 字节的 Android 请求头，不会修改直接 lookup 请求；ioctl 回退的
`process_transaction()` 也只为 `listServices` / `getServiceDebugInfo` 建立待处理回复状态，不改写请求。过滤边界放在
这两类回复，因此批量枚举仍被隐藏，而框架直接
查询得到的 Binder 不会被强制变成 null。

**结论：安全（已修复）。** 回归时应分别调用 `getService("profile")`、
`checkService("profile")`，确认调用链和非目标服务不受影响。

## 2. `ServiceManager.sCache` 清理

`postAppSpecialize` 中清理一次缓存是必要的，但缓存可能在后续 lookup 中重新填充。
当前 `clear_cache()`：

- 对 `FindClass`、字段/方法查找、Java 调用逐步检查异常；
- 使用 `keySet().toArray()` 后再删除，避免迭代期间修改 Map；
- 释放所有 LocalRef；
- 仅在 `postAppSpecialize` 执行一次；不在 Binder 事务回调中并发修改 `ArrayMap`。

不会把 `sCache` 置 null，也不会阻止框架初始化；删除的仅是匹配 ROM 关键字的条目。

**结论：基本安全。** 某些 ROM 可能将 `sCache` 改为不可变 Map，此时调用会抛出
异常；代码会清除异常并继续，不会让应用崩溃，但缓存条目可能保留。若需覆盖该类
ROM，应增加只读 Map 的复制替换方案，并在独立设备上验证。

## 3. `FORCE_DENYLIST_UNMOUNT`

该选项用于目标进程进入 Magisk denylist 的挂载视图，隐藏模块文件和其他 Magisk
挂载。它不会改变普通 `/system`、`/vendor`、应用数据目录的访问权限，但依赖
Magisk Overlay、root 注入库或第三方挂载资源的目标应用可能出现资源缺失。

当前只对 `is_target()` 成功且 `force_denylist_unmount=true` 的进程调用，系统进程和非目标进程不受影响。
配置缺省值仍为 `true`，因此不改变既有隐蔽行为；依赖 Magisk Overlay、root 注入库或第三方挂载资源的目标应用
可将该字段设为 `false`，关闭后服务过滤和 `sCache` 清理仍然生效。

**结论：已缓解（仍需设备验证）。** 不是普遍启动崩溃点，但应在启用和关闭两种模式下分别回归。

## 4. 等长替换和 Parcel 边界

ioctl 回退的 `process_string16()` 在读取长度、终止符、ASCII 范围、4 字节对齐和
总大小前均检查，`offsets_size` 使用 8 字节对齐后的独立区域复制；超过 256 KiB 或
缓冲区正在使用时跳过交换，不会写入原 Binder 缓冲区。

JNI 路径使用 Java `Parcel.readString/writeString`，只把相同 UTF-16 长度的 ASCII
服务名替换为下划线。回复先读取 `readException()` 头，再处理数组；异常、负数/超大
计数或 malformed payload 均恢复位置并跳过。

**结论：安全。** 仍建议在 fuzz/回归测试中覆盖截断 Parcel、空字符串、非 ASCII
服务名、含 Binder fd 的 offsets 区域，以及大于 256 KiB 的回复。

另外，回退路径的回复交换曾将 `BC_FREE_BUFFER` 指针改为 0；这会阻止 Binder
驱动释放原始 mmap 缓冲，重复枚举后可能耗尽进程可用的 Binder 缓冲。现在交换时
保存原始指针，收到对应的 `BC_FREE_BUFFER` 后恢复该指针再交给驱动，匿名替换
缓冲仍由线程本地内存复用。

## 5. protected package 覆盖

`action.sh` 只枚举第三方包，但用户也可以手工编辑 config。当前 `is_target()` 除
`android`、`system`、`system_server`、SystemUI、Settings 外，增加了
PermissionController、PackageInstaller、设置/媒体/下载/联系人等关键 Providers、DocumentsUI、电话、
Telecom、蓝牙、NFC、输入法等包，避免这些组件在启动早期被过滤。

**结论：已加强。** ROM 若使用不同包名，应把对应的早期系统组件加入同一保护数组；
不要用简单的 `com.android.*` 全前缀保护，否则会改变用户显式配置第三方系统扩展的
行为。

## 6. 进程生命周期和 Hook 安装

- 非目标进程在 `preAppSpecialize` 直接设置 `DLCLOSE_MODULE_LIBRARY`，没有安装
  Hook；
- 目标进程设置 `FORCE_DENYLIST_UNMOUNT`，在此之前完成 JNI Hook 注册；
- JNI Hook 保存原始 `fnPtr`，目标进程不再调用 Zygisk API；
- JNI Hook 失败时才进入 ioctl 回退；
- 目标进程不调用 `DLCLOSE_MODULE_LIBRARY`，避免回调指针悬空。

**结论：安全。** 进程退出时由系统回收映射和 TLS；不会执行主动卸载或恢复 GOT 的
高风险操作。

## 7. `action.sh` 及配置生成

旧脚本无条件将当前用户的全部第三方应用写入 targets，并在空列表时依赖行数判断
逗号；还会覆盖手工配置，无法覆盖工作资料。

现在的脚本：

- 枚举 `cmd user list`、`pm list users` 和当前用户，覆盖 owner、secondary user
  及 work profile；
- 交互模式支持 `a` 全选合并、`s` 序号合并、`r` 序号替换、`k` 保留、`q` 取消；
- 无 TTY 或空输入默认保留现有 targets；
- 健壮读取 `enabled`，处理空列表；
- 临时文件与 live config 同目录，写入/校验/chmod 成功后 `mv -f` 原子替换；`post-fs-data.sh` 首次创建配置也采用同样策略；
- 任何失败通过 trap 清理临时文件，不删除旧配置。

**结论：已修复。** 在设备上应测试没有 `/dev/tty`、没有第三方包、只有工作资料、
以及 config 为单行 JSON 的场景。

## 建议的最小回归矩阵

1. 冷启动目标应用，观察 logcat 无 NPE、`SecurityException` 或 native tombstone；
2. 调用 `getService/checkService("profile")`，确认不返回被强制替换的 null；
3. 调用 `listServices`、`getServiceDebugInfo`，确认匹配项不可见、普通项完整；
4. 读取 `ServiceManager.sCache`，确认匹配 key 不存在；
5. 在 owner + work profile 下运行 action，确认 targets 合并且旧手工项保留；
6. 分别打开/关闭 denylist unmount，验证依赖 Magisk 挂载的应用资源可用性；
7. 对 release so 执行 `readelf -Ws`、`/proc/<pid>/maps` 和 GOT/dladdr 检查。

## 8. 插桩可观察性和验证

| 特征 | 旧 PLT 路径 | 当前实现 |
| --- | --- | --- |
| `libbinder` GOT 槽归属模块代码 | 直接指向模块回调 | JNI 主路径不改 GOT；旧系统回退指向匿名 RX 跳板 |
| 模块私有符号 | 可能出现在 ELF 符号表 | `exports.map` 仅导出 `zygisk_module_entry`，Release 运行 strip |
| `rwxp` 映射 | 可能出现 | 跳板写入后立即 `mprotect(PROT_READ|PROT_EXEC)` |
| C++ `atexit`/析构痕迹 | 全局对象可能注册 | 进程配置故意泄漏，避免退出析构；线程缓冲由 pthread key 回收 |
| `/proc/self/maps` 模块路径 | 可见 | JNI 回调必须驻留，模块文件映射仍可能可见；强行卸载会使回调悬空并增加崩溃风险 |

建议在设备或 CI 产物上执行：

```bash
unzip -p out/Yukari.zip zygisk/arm64-v8a.so >/tmp/yukari.so
readelf -Ws /tmp/yukari.so
adb shell 'cat /proc/$(pidof <target>)/maps | grep -E "yukari|rwxp"'
```

### Transaction compatibility note

`IServiceManager` transaction IDs are not constant across all Android releases. The
implementation reads `Build.VERSION.SDK_INT` and uses list/debug slots `4/12`,
`4/13`, `4/14` on Android 12/13/14, and `6/16` on the newer AIDL layout (Android
16 and later). Android 11 and earlier do not enable debug-info filtering in the
ioctl fallback because transaction 10 is a different operation on those builds.

JNI 主路径下 `ioctl` GOT 应保持原始 libbinder 地址；仅启用旧系统回退时，使用
`dladdr` 检查 GOT 槽落在匿名 `r-xp` 跳板映射。由于回调代码仍驻留模块库，当前方案
不会宣称 `/proc/self/maps` 完全无模块路径；这是在低崩溃风险和跨版本兼容性下的明确取舍。

`clear_cache()` 只在 `postAppSpecialize` 执行一次。事务回调不再并发修改
`ServiceManager.sCache`：AOSP 的 `getService()` 对 miss 不会回填该 Map，重复清理的
收益有限，反而可能与应用线程的 `ArrayMap` 读取产生竞态。若厂商 ROM 明确会动态回填，
应优先实现受锁保护的代理 Map，并在该 ROM 上单独回归。

`preServerSpecialize` 现在对 `system_server` 设置 `DLCLOSE_MODULE_LIBRARY`。该进程不
安装任何 hook，因此卸载不会留下悬空回调，也能减少非目标进程的模块映射。

安装脚本的旧配置备份现在写入 Magisk 提供的 `MODPATH`，不依赖 recovery 是否导出
`TMPDIR`；备份失败时不会假报“已保留”，避免升级后误覆盖用户配置。

JNI/native 可选路径对初始化、Parcel 字符串和回退安装增加了异常兜底；资源不足时过滤
会降级为不安装回退 hook，而不会让 C++ 异常穿过 JNI 边界终止目标应用。
