# Yukari

[English](README.md) | [中文](README.zh-CN.md)

---

Zygisk 模块，用于隐藏指定目标应用的自定义 ROM 服务信号。

### 固定服务关键词

匹配方式为 ASCII 大小写不敏感，并内置在模块中：

- `lineage`
- `crdroid`
- `aospa`
- `pixelexperience`
- `omnirom`
- `protonaosp`
- `profile`

用户可配置目标应用和可选过滤通道。模块不会全局停止或注销服务；过滤仅作用于选定的应用进程。

### 当前 native 行为

- Zygisk 应用特化时匹配配置的目标包名。
- 包名规则同时覆盖 `com.example.app:remote` 等私有子进程；包含冒号的进程规则
  仍为精确匹配。系统 appId 和受保护框架包（含其子进程）不会注入。
- 移除 Java `ServiceManager.sCache` 中包含固定 ROM 关键词的条目。
- Binder 请求过滤：验证 ServiceManager 接口后，仅在私有副本中改写首个服务名
  参数。原始请求和写命令保持不变，部分写入重试也不会污染调用者的数据。
- Binder 回复过滤：使用安全的缓冲区交换机制，扫描并过滤所有 ServiceManager 回复（包括 `listServices` 和 `getServiceDebugInfo`）。阻止应用枚举隐藏的服务。
- Binder 拦截使用 Zygisk 的标准 PLT hook API，而非内联修补 libc。

请求/回复过滤使用等长占位符替换，不改变 Parcel 大小。

服务名直接就地匹配，无需构造临时字符串。未命中隐藏服务的回复保留原始
Binder 缓冲区；命中的回复使用按需增长、可复用的线程本地缓冲区，上限仍为
256 KiB。释放过滤后的回复时，也会将原始缓冲区归还 Binder 驱动。

### 配置

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

`hide_features` 为目标进程启用精确 Lineage feature 过滤，默认关闭。查询使用
私有 Parcel；枚举仅替换已登记的 PackageManager 回复所读取的字符串。日志分别
报告查询与枚举是否可用；JNI 方法或生命周期 ABI 不支持时，对应能力保持可见。
覆盖 Java 调用和首次枚举回复；ParceledListSlice 延迟分片、未知回复头、主动重设
Parcel 位置时保守放行。`action.sh` 重新生成目标列表时保留此选项。

`hide_resources` 独立过滤 `lineageos.platform` 的资源 ID 查询、单资源值和名称
反查，默认关闭。必须由当前 AssetManager 的包映射确认后才过滤 ID，不会把
无关的 `0x3f` 包当成 Lineage；映射缺失时 ID 访问保持可见。资源数组、theme
和包映射列表不在此通道范围内。`action.sh` 同样保留此选项。

### 构建

```bash
bash scripts/build.sh test     # 宿主测试，不需要 Android SDK
bash scripts/build.sh android  # SDK 准备、Gradle 编译、ZIP 验证
# 或完整执行：bash scripts/build.sh all
```

Android 构建需 JDK 17，以及带命令行工具的 Android SDK；设置 `JAVA_HOME` 和
`ANDROID_HOME`（或 `ANDROID_SDK_ROOT`）。官方 Wrapper 固定 Gradle 8.11.1 并
校验下载哈希；SDK、NDK、CMake 版本统一写在 `gradle.properties`。产物为
`out/Yukari.zip`，详见[构建说明](docs/build.md)。

CI 将宿主验证与 Android 打包拆为独立任务。每次显示实际构建提交，并将阶段
日志保留在 `out/diagnostics/`。修改后需推送新提交触发新运行；对旧 Actions
运行点“重新运行”，仍会使用旧提交和旧工作流。

可在具有 C++20 编译器、CMake、make、binutils 和 JDK JNI 头文件的 Linux/WSL
环境运行回归测试，详见[测试依赖与覆盖范围](tests/native/README.md)：

```bash
bash tests/native/run.sh
```

各通道在特化前独立安装，特化后日志分别报告 `services`、`cache`、
`feature_query`、`feature_enum`、`resources` 的 `off`、`ready`、`partial`
（仅资源）或 `unavailable` 状态。状态代表安装结果，不代表已通过设备检测；
缓存清理失败也不会关闭其他已安装通道。

可运行 `bash scripts/diagnose_device.sh 包名 [adb序列号]` 收集只读加载诊断。
真实 API 行为按[设备验收清单](tests/device/README.md)逐项验证；宿主测试与
JNI 模拟测试不能替代 Android ART、Zygisk 和驱动上的实测。
