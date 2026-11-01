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
- Binder 请求过滤：在请求到达 ServiceManager 之前，重写匹配的服务查询名。
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

### 构建


```bash
gradle :module:assembleRelease
bash scripts/package.sh
```

可在具有 C++20 编译器的 Linux 环境运行 native 回归测试：

```bash
bash tests/native/run.sh
```
