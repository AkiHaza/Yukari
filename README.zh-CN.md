# Yukari

Yukari 是一个按目标应用生效的 Zygisk 模块，用于隐藏自定义 ROM 的
ServiceManager 服务信号。固定匹配关键字为 `lineage`、`crdroid`、`aospa`、
`pixelexperience`、`omnirom`、`protonaosp`，并精确匹配 `profile`。

## 实现策略

目标进程在 `preAppSpecialize` 注册 `android.os.BinderProxy.transactNative`
的 JNI hook，并在 `postAppSpecialize` 清理一次 `ServiceManager.sCache`。该路径在 Parcel
层完成过滤，不改写 `libbinder.so` 的 PLT/GOT：

- 读取运行时 `IServiceManager.Stub.TRANSACTION_*`，兼容新 Lineage 插入
  `getService2`/`checkService2` 后的事务编号变化，不仅根据 SDK_INT 猜测；
- `getService`/`checkService` 及其 `*2` 版本仅对明确来自应用的匹配查询进行等长
  名称替换；先复制请求 Parcel，保留调用方原始内容与位置；
- 调用栈跳过 Binder/ServiceManager 和反射转发帧，以第一个实际调用方判定；
  框架/Lineage 调用、无法识别的调用和分类失败均保留真实 Binder，避免初始化 NPE；
- `listServices` 的 `String[]` 回复被过滤并以相同 UTF-16 长度写回；
- `getServiceDebugInfo` 的 `ServiceDebugInfo[]` 名称被过滤；
- 直接使用 `transactNative` 传入的 Java `Parcel` 对象，不接管 native 所有权；
  因此不会触碰 `mNativePtr` 或触发额外的 native 释放。

在极旧系统上，如果 JNI 方法签名不可用，则回退到 ioctl 过滤。回退路径仍然
使用原有的安全缓冲区交换，并通过匿名、RX-only 跳板作为 PLT 替换地址，避免
GOT 槽直接指向模块 `.text`。
该回退路径仍只过滤枚举/调试回复，不对直接 lookup 做调用方分类或重写。

`sCache` 仍仅在 specialization 时清理一次，不在事务回调中并发修改 Map。
核对的 Lineage `getCommonServicesLocked` 不向应用预填 `profile`，普通 lookup miss
也不会回填 `sCache`；厂商动态注入匹配缓存、纯 native libbinder 直查，以及通过
受信任框架/Lineage API 间接获取服务仍可能绕过当前边界。前缀保护是启动兼容策略，
不是强制安全边界；不能承诺任何检测方式均不可见。

## 可观察特征取舍

| 特征 | 旧实现 | 当前实现 |
| --- | --- | --- |
| `ioctl` GOT 槽指向模块代码 | 是 | JNI 路径：否；旧系统回退：指向匿名跳板 |
| PLT/GOT 被改写 | 是 | JNI 路径：否 |
| `rwxp` 映射 | 可能出现 | 跳板创建后立即 `mprotect` 为 `r-x` |
| C++ 全局析构/`atexit` | `std::string`/配置对象可能注册 | 进程状态改为平凡可析构对象；配置故意驻留 |
| `.symtab`/私有符号 | 可能存在 | CMake 版本脚本 + `--strip-unneeded`，仅保留 Zygisk 入口 |
| `/proc/self/maps` 中模块路径 | 可见 | JNI hook 仍需驻留回调代码，因此路径仍可能可见 |

模块路径匿名化或在保留回调的同时 `dlclose` 会使函数指针悬空，带来高崩溃
风险，因此没有强行执行。需要做到“maps 完全无模块路径”时，必须把完整回调
运行时（代码、只读数据、TLS 和异常处理）搬迁到独立 ELF/匿名映射后再卸载，
这超出了安全的跨 Android 版本实现范围。

## 配置

```json
{
  "enabled": true,
  "force_denylist_unmount": true,
  "targets": ["com.example.app"]
}
```

仅列出的应用进程会启用过滤，系统进程和受保护包始终跳过。
如果目标应用依赖 Magisk 挂载的文件或资源，可将 `force_denylist_unmount` 设为
`false`；服务过滤仍然生效。

运行模块 action 可通过序号合并或替换 targets，`a` 为全选合并、`k` 保留、`q` 取消。
Magisk 管理器没有终端时，音量上键全选合并、音量下键进入逐包选择；超时保持原文件。
脚本验证三个已知配置字段；未知字段或不支持的 JSON 转义会中止写入并保留原配置。

## 分阶段验证

对应的实现也可以分阶段启用：

- 仅保留 `service_cache.cpp`，验证 `sCache` 清理；
- 启用 `BinderProxy.transactNative` JNI hook，验证列表/调试回复；
- 在不支持该 JNI 签名的旧系统上启用 `install_hooks()` ioctl 回退；
- 最后打开 CMake 的 strip/version-script 检查，确认发布 ELF 不含私有符号。

1. **构建检查**

   本地需要 JDK 17、Gradle 8.11.1 和 Android SDK/NDK。仓库的 `gradlew` 是调用
   `PATH` 中 Gradle 的入口；CI 使用 `setup-gradle` 安装固定版本。
   CI 的 SDK setup 显式只安装 `platform-tools`，避免 action 默认安装已下架的
   `tools` 包导致编译之前失败；SDK 36、NDK 和 CMake 仍由后续步骤固定安装。

   ```bash
   ./gradlew :module:assembleRelease
   bash scripts/package.sh
   ```

2. **ELF 符号检查**（设备或 CI 主机）

   ```bash
   unzip -p out/Yukari.zip zygisk/arm64-v8a.so >/tmp/yukari.so
   readelf -Ws /tmp/yukari.so
   # 预期：仅有 zygisk_module_entry 动态导出，不出现 hook_ioctl 等私有符号
   ```

3. **运行时 GOT 检查**

   在目标进程中读取 `libbinder.so` 的 `ioctl` 槽，并用 `dladdr` 检查归属。
   JNI 主路径不会修改该槽；旧系统回退路径的地址应落在匿名 `r-xp` 跳板映射。

4. **映射检查**

   ```bash
   adb shell 'cat /proc/$(pidof your.target)/maps | grep -E "yukari|rwxp"'
   ```

   不应有 `rwxp`；JNI 路径下仍可能看到模块的只读/可执行文件映射（见上表）。

5. **功能回归**

   在目标应用中调用 `getService("profile")`、`checkService`、
   `listServices` 和 `getServiceDebugInfo`，并覆盖新的 `getService2`/`checkService2`。
   批量枚举和调试信息中的匹配项应不可见；应用直接 lookup 匹配项应拿不到服务，
   框架启动和 Lineage 内部查询应仍获取真实 Binder。不要通过外部 `adb shell service list`
   判断过滤结果：它不在目标应用进程内。非匹配项、非目标应用均保持正常。
   同时验证请求 `dataSize/dataPosition` 和字节内容未改变、真实 Binder 异常仍正常传播。
   确认 logcat 的 `SM transactions` 日志与该 ROM 的实际事务号一致；启动阶段不得出现
   NPE 或 native 崩溃。
