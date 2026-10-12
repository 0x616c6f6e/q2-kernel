# Q2-Kernel

面向小型设备的可移植 C11 应用内核，版本 **0.1.0 / Lua API 2**。负责组件生命周期、依赖关系、事件分发、权限和 Lua runtime。底层 RTOS 仍负责线程、抢占和中断，Q2-Kernel 管理应用与组件的协作执行。

本目录现在是独立 Git 仓库，可以独立复制和构建，不依赖 ESP-IDF、Q2-App、Q2-Platform 或旧 firmware 的源码。产品适配由独立 Q2-Platform 消费本目录；应用由独立 Q2-App 生成。

## 开始

需要 C11 编译器、CMake 3.22+；脚本测试需要 Python 3.10+。第三方源码已固定在仓库内，构建不会联网下载。

```sh
cmake -S . -B build/native -DQ2_BUILD_TESTS=ON -DQ2_WITH_SQLITE=ON -DQ2_WITH_USB=ON
cmake --build build/native
ctest --test-dir build/native --output-on-failure
# 或使用 GCC/Clang/Zig 的 UBSan 集成验证
python scripts/build.py --cc clang
python scripts/check_contracts.py
```

Windows 推荐 Ninja + Clang 或 Zig；Zig 配置增加 `-DCMAKE_C_COMPILER=/path/to/zig.exe -DCMAKE_C_COMPILER_ARG1=cc`。MSVC 尚未验证。不启用测试时 USB 由实际 SoC 提供 DCD 实现。

## 目录

```text
include/q2/        对外 C 契约及 API 注释
src/core/         与 Lua、SDK 无关的生命周期、事件及授权
src/runtime/      manifest 校验、Lua VM、通用 API 桥接
components/       apps / input / data / network / storage / sqlite / usb / display
third_party/      固定版本的上游源码、来源、许可证、文件校验表
cmake/            可复用源码清单
scripts/          独立构建与架构边界检查
tests/            契约及真实组件集成测试、测试用 USB 控制器
docs/             设计、移植、API、开发与验证说明
ai/               AI 可读的能力清单
```

## 文档导航

| 文档 | 内容 |
|---|---|
| [架构](../docs/architecture.md) | 三层边界、调度、资源所有权 |
| [SoC 接入](docs/porting.md) | 三个必需回调与外设扩展 |
| [组件开发](docs/components.md) | 统一注册、生命周期、权限和返回值 |
| [Lua API](docs/lua-api.md) | 当前可调用的方法与运行限制 |
| [日志](docs/logging.md) | 编译/运行开关、等级、平台后端和 Lua 日志 |
| [LVGL Display](docs/lvgl-display.md) | 可选图形 provider、Lua 控件与像素后端 |
| [开发规范](../docs/development.md) | 编码、注释、变更及验证要求 |
| [AI 协作](../ai/kernel.md) | AI 工作流程及机器可读契约 |
| [迁移与路线](../docs/migration.md) | 已实现范围、旧固件迁移顺序 |
| [验证记录](../docs/validation/kernel-baseline.md) | 实际构建和测试证据 |
| [第三方依赖](third_party/README.md) | 版本、来源、许可证、配置改动 |

当前是可执行、可构建的独立内核基础版，已增加可选 LVGL 图形组件和 Windows PC 窗口适配。播放器完整功能仍保留在旧 firmware；USB 实体控制器、实机屏幕驱动、音频、无线、安装器及 OTA 尚未迁入新架构，详见迁移表。

前台 APP 管理、出厂主页和共享背景见 [应用管理](docs/application-manager.md)。可选测试开关 `Q2_BUILD_APPS_TESTS=ON`。

[按键注册与事件透传](docs/keys.md)：按名称注册，SINGLE/DOUBLE/LONG 三种事件，组件不承担按键识别或功能映射。测试开关 `Q2_BUILD_KEYS_TESTS=ON`。

公共架构/开发/AI 文档已集中到父级 q2，单独克隆 Kernel 时可直接使用本目录 CMake、公开头文件、API/组件文档及契约检查；它们不读取父级配置。

[Network](docs/network.md)：可选异步 HTTP/HTTPS 文本 GET，传输由 Platform 注册，当前有 Windows WinHTTP 后端。

音频播放组件见 [Audio](docs/audio.md)：Kernel 管理权限与 APP 所有权，平台注册异步播放后端。
独立契约测试开关为 `Q2_BUILD_AUDIO_TESTS=ON`；Windows 后端位于 Q2-Platform。
