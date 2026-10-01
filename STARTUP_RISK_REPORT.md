# Yukari 启动失败风险排查报告

本报告基于本地 `dev` 分支当前源码（`module/src/main/cpp/`、模块脚本和构建脚本）
整理。结论按“安全 / 有隐患”给出；能在不改变模块目标范围和过滤功能的地方已直接
修复。

这里的“安全”表示源码检查未发现该项原有的确定性崩溃路径，不代表已通过真机验收。
本机缺少 Gradle、Android SDK/NDK，且没有 adb 设备；编译、服务回归、maps/GOT 检查仍待执行。

## 结论摘要

| 检查项 | 结论 | 处理 |
| --- | --- | --- |
| Binder 直接 lookup 误伤 | 安全（已修复） | JNI/ioctl 路径均不改写 `getService`/`checkService` 请求 |
| `sCache` 清理时机 | 基本安全 | 仅在 `postAppSpecialize` 清理一次，避免事务回调并发修改 `ArrayMap`；检查 JNI 异常与 key 类型 |
| `FORCE_DENYLIST_UNMOUNT` | 已缓解（仍需设备验证） | 仅目标进程启用；新增 `force_denylist_unmount` 配置开关，默认 `true`，兼容性问题可关闭 |
| 等长 String16 替换 | 安全 | 所有 offset/长度/对齐均做边界检查；过大或 malformed Parcel 跳过 |
| ioctl 回复缓冲释放 | 已缓解，仍有隐患 | 转发原始驱动指针、失败重试保留交换状态；跨线程 Parcel 生命周期仍需验证 |
| JNI Parcel 回复重写 | 已修复，待设备验证 | 保留回复头、数组计数、AIDL parcelable 长度头及 `ServiceDebugInfo.debugPid` |
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
- 检查 key 是否为 String，内存不足时停止迭代并清除此次清理产生的异常；
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
服务名替换为下划线。Android 8/9 的枚举回复为单个 String16，Android 10 起处理
无异常回复头后的 String 数组。调试数组逐项读取 presence、AIDL parcelable 大小、
name 和 debugPid，并跳过未知尾部字段。写回仅覆盖原字符串，不重建整个数组；
异常、超大计数或截断字段停止解析，并恢复回复位置。

**结论：安全。** 仍建议在 fuzz/回归测试中覆盖截断 Parcel、空字符串、非 ASCII
服务名、含 Binder fd 的 offsets 区域，以及大于 256 KiB 的回复。

另外，回退路径的回复交换曾将 `BC_FREE_BUFFER` 指针改为 0；这会阻止 Binder
驱动释放原始 mmap 缓冲，重复枚举后可能耗尽进程可用的 Binder 缓冲。现在交换时
保存原始指针，收到对应的 `BC_FREE_BUFFER` 后恢复该指针再交给驱动，匿名替换
缓冲仍由线程本地内存复用。只在 ioctl 成功且驱动确认消费释放命令后清除状态；
失败时恢复命令中的交换指针，允许 libbinder 重试。命令结构通过 memcpy 读取，避免
4 字节命令头后的非自然对齐结构访问。

**剩余隐患：** ioctl 回退仍使用线程本地交换缓冲。若 OEM 把回复 Parcel 跨线程
释放，或其生命周期超过接收线程，交换指针的回收跟踪可能失效。JNI 主路径不接管
native Parcel 所有权，不存在该交换缓冲问题；覆盖这种 OEM 行为需要独立的跨线程
缓冲所有权表及设备回归。

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
- 目标进程仅在配置 `force_denylist_unmount=true` 时设置 `FORCE_DENYLIST_UNMOUNT`，
  并在 `preAppSpecialize` 注册 JNI Hook；
- `ServiceManager.sCache` 仅在 `postAppSpecialize` 清理一次；事务回调不再清理缓存；
- JNI Hook 保存原始 `fnPtr`，目标进程不再调用 Zygisk API；
- JNI Hook 失败时才进入 ioctl 回退；
- 目标进程不调用 `DLCLOSE_MODULE_LIBRARY`，避免回调指针悬空。

本地 API 包装在 JNI hook 函数不存在时将 fnPtr 清空，避免把未调用的 API 误判为成功。
成功安装仍依赖 Zygisk 按公开契约返回原始 native 函数。如果非标准实现已经改写
native 入口却不返回原指针，公开 API 没有恢复原函数的手段；该异常实现不能承诺安全。

**结论：安全。** 进程退出时由系统回收映射和 TLS；不会执行主动卸载或恢复 GOT 的
高风险操作。

## 7. `action.sh` 及配置生成

旧脚本无条件将当前用户的全部第三方应用写入 targets，并在空列表时依赖行数判断
逗号；还会覆盖手工配置，无法覆盖工作资料。

现在的脚本：

- 枚举 `cmd user list`、`pm list users` 和当前用户，覆盖 owner、secondary user
  及 work profile；
- 交互模式支持 `a` 全选合并、`s` 序号合并、`r` 序号替换、`k` 保留、`q` 取消；
- 无 TTY 时支持音量键全选/逐包合并；超时、不可用输入或空选择保持原文件；
- 通过 POSIX awk 解析顶层配置，保留两个布尔开关和手工 targets；非法 JSON、重复键、
  未知字段或不支持的转义中止更新，避免默默丢失手工配置；
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

`IServiceManager` 的事务号按 AOSP Android 11–16 标签中的 AIDL 方法顺序核对：
`listServices` 始终为 4；`getServiceDebugInfo` 在 Android 12/13/14–16
分别为 12/13/14。Android 11 及更早版本没有该调试方法，回退路径不猜测其事务号。
厂商若修改 AIDL 顺序，应按实际 ROM 源码验证后再适配，避免把其他服务回复当成列表改写。

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
`TMPDIR`；备份或原子恢复失败时中止安装，避免用户选择保留却被替换为默认配置。

JNI/native 可选路径对初始化、Parcel 字符串和回退安装增加了异常兜底；资源不足时过滤
会降级为不安装回退 hook，而不会让 C++ 异常穿过 JNI 边界终止目标应用。

## 9. 本次验证与文件完整性

- 开始检查时 `module/` 的 22 个跟踪文件缺失，现已从当前 dev 提交恢复，其他文件未被覆盖。
- 35 个跟踪文件均存在、非空、UTF-8 解码通过；源码/脚本为 LF，默认 JSON 和 Manifest 可解析。
  `git fsck` 未发现损坏对象，输出的 dangling 对象是未被当前分支引用的历史对象。
- ShellCheck（POSIX sh / Bash）、dash/sh/bash 语法检查和 actionlint 工作流检查通过。
- 配置解析的 13 项回归通过：单行/多行 JSON、默认字段、重复键、嵌套/伪造字段、截断数组、
  非法值和转义；11 项 action 冒烟覆盖全选、序号合并/替换、多用户、空包列表、取消保留，
  以及写入/chmod/mv 失败仍保持旧配置字节不变。
- 工作流和本地 ELF 检查不再依赖 `set -e` 执行独立的 `! pipeline`；显式失败并验证唯一
  已定义入口。三项模拟符号表用例通过（正常、私有 C++ 符号泄露、只有未定义入口）。
- `gradlew` 是 PATH Gradle 的轻量入口，Git 中具有执行位；CI 已安装 Gradle 8.11.1。
- 没有可用的本地 release so/zip；不能据此宣称 ELF、应用启动或运行时隐蔽性验收通过。
