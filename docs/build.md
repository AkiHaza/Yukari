# 构建流程

## 环境与版本

Linux/WSL 是脚本支持的构建环境。宿主验证需 Git、Bash、C++20 编译器、
Python 3.9+、CMake、make、binutils 和 JDK JNI 头文件；Android 构建需 JDK 17
和 Android SDK 命令行工具。
GitHub 的 Ubuntu runner 已提供这些基础工具，构建不再调用 setup-android action。

- Gradle 版本和分发 SHA-256：`gradle/wrapper/gradle-wrapper.properties`。
- AGP 版本：根目录 `build.gradle.kts`。
- compile/target/min SDK、Build Tools、NDK、CMake：`gradle.properties`。
- SDK 准备脚本和 Gradle 读取相同的版本属性，避免两处漂移。

本地示例（路径替换为实际安装位置）：

```bash
export JAVA_HOME=/path/to/jdk-17
export ANDROID_HOME=/path/to/android-sdk
export ANDROID_SDK_ROOT="$ANDROID_HOME"
bash scripts/build.sh all
```

两个 SDK 环境变量同时设置时必须指向同一目录。SDK 中需先有
`cmdline-tools/latest/bin/sdkmanager` 或带版本号的命令行工具；脚本不下载未知
来源的初始化程序。Wrapper 使用仓库指定的官方分发地址及哈希，不需要全局 Gradle。

## 统一入口

| 命令 | 行为 |
| --- | --- |
| `bash scripts/build.sh test` | native 回归与构建基础设施测试，不准备 SDK。 |
| `bash scripts/build.sh android` | 检查 JDK、准备 SDK、使用 Wrapper 编译 release、打包并验证。 |
| `bash scripts/build.sh all` | 先测试，再执行 Android 构建；无参数时默认此模式。 |
| `bash scripts/build.sh package` | 使用已有 Gradle 编译产物重新打包。 |
| `bash scripts/android-sdk.sh --check` | 仅校验现有 SDK 元数据与所需工具，不安装或接受许可。 |
| `bash scripts/verify_package.sh out/Yukari.zip` | 独立校验 ZIP 结构、模块属性与 arm64 ELF。 |

SDK 脚本只请求项目声明的五类包，不请求旧 `tools` 包。校验读取安装元数据
及工具文件，不依赖 SDK CLI 的表格输出格式。已满足依赖时跳过重复安装。
需要安装时会接受 SDK 许可；失败保留真实命令退出码，避免 `yes` 的 SIGPIPE
或过量进度输出掩盖错误。

## 打包与诊断

默认产物为 `out/Yukari.zip`。也可显式指定库和输出：

```bash
bash scripts/package.sh --library /path/to/libyukari.so --output /path/to/Yukari.zip
```

打包使用输出目录内的隔离暂存目录。只有完整验证成功，才原子替换目标 ZIP；
失败不破坏已有 ZIP。CI 的版本号只写入暂存 `module.prop`，不修改仓库原文件。
ELF 验证要求 AArch64 动态库且仅导出 Zygisk 入口；ZIP 验证检查必要文件、
重复/危险路径、CRC 和模块属性。

每次统一入口运行创建 `out/diagnostics/<模式>.<随机ID>/`，记录源码提交、
请求 ref/SHA、工作区状态及各阶段日志。失败时输出对应目录，旧日志不会冒充
本次阶段日志。Android 任务另外上传 AGP/CMake 原始诊断（如果已生成）。

## GitHub Actions

`host-tests` 先验证 native 与构建脚本；`android-build` 依赖它成功后再准备
SDK、编译和打包。main、dev、codex/** 推送，以及 pull request 和手动触发均可
运行。实际 checkout 必须与事件的 `GITHUB_SHA` 一致。

重新运行历史任务不会切换到新代码。验证修复时应推送包含修复的新提交，或在
手动触发时选择正确分支，并核对日志中的完整提交哈希。下载的日志只作为诊断
数据，不作为脚本执行来源。
