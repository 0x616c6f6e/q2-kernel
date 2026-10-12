# 固定依赖与许可

源码随独立 Kernel 分发，构建不联网拉取。`downloads.json` 记录本次下载归档 URL/版本/SHA-256，已有来源信息保留于对应 `upstream.json`；`files.sha256.json` 对实际纳入的文件逐一校验。

| 目录 | 版本 | 来源 | 许可依据 |
|---|---|---|---|
| lua | 5.5.1 | https://www.lua.org/ftp/lua-5.5.1.tar.gz | lua/LICENSE.html 及 lua.h 文件头/尾声明，MIT |
| cjson | 1.7.19 | https://github.com/DaveGamble/cJSON/tree/v1.7.19 | cjson/LICENSE，MIT |
| littlefs | 2.11.3 | https://github.com/littlefs-project/littlefs/tree/v2.11.3 | littlefs/LICENSE.md，BSD-3-Clause |
| fatfs | R0.15，源码标注 w/patch1 | https://elm-chan.org/fsw/ff/arc/ff15.zip | ff.c/ff.h 文件头的 ChaN 许可，保留原文 |
| tinyusb | 0.18.0 | https://github.com/hathach/tinyusb/tree/0.18.0 | tinyusb/LICENSE，MIT；各后端文件也保留自己的声明 |
| sqlite | 3.50.4 | https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip | SQLite 源码声明 public domain；参见 https://www.sqlite.org/copyright.html |
| lvgl | 9.3.0 | https://github.com/lvgl/lvgl/tree/v9.3.0 | lvgl/LICENCE.txt，MIT；保留源码内各子模块原声明 |

Lua/cJSON/LittleFS 来源及校验继承自已保存的 upstream 记录；下载归档和实际纳入文件的校验含义不同。并不声称这些固定版本始终是最新版本。

## 本地配置

FatFs `ffconf.h` 相对下载包调整：`FF_USE_MKFS=1`、`FF_CODE_PAGE=437`、`FF_USE_LFN=1`、`FF_FS_NORTC=1`。启用格式化用于显式操作；长文件名承载 UUID 目录；没有硬件 RTC，文件时间不代表真实时间。实际文件以校验表锁定。

Lua 限制 `LUAI_MAXCCALLS=32`、`LUAI_MAXSTACK=2048`；只编译需要的标准库，裁剪逻辑在第一方 runtime 内。

SQLite 构建设置 `SQLITE_OS_OTHER=1`、`SQLITE_THREADSAFE=0`、`SQLITE_OMIT_LOAD_EXTENSION=1`、`SQLITE_TEMP_STORE=3`、`SQLITE_DEFAULT_MEMSTATUS=0`。内存 VFS 在第一方 components/storage 中，不改上游 amalgamation。

TinyUSB 只链接 device/CDC/common 基础源码，目标 DCD 由 SoC 提供。配置文件在第一方 components/usb 中。其他纳入的上游后端并不等于已验证兼容所有目标。

升级步骤：核对上游来源和许可 → 保存归档 SHA-256 → 应用明确配置 → 更新文件锁 → 编译及集成测试 → 记录行为/资源变化。不要只重生成锁来掩盖不明改动。项目第一方代码的发布许可证仍待选择。

LVGL 从官方 v9.3.0 归档纳入 src、顶层头文件、配置模板及许可/说明。上游源码未修改，第一方配置位于 components/display/lv_conf.h，采用无 OS 的软件渲染；不启用上游 Windows/SDL/RTOS 驱动。Windows 后端由独立 Q2-SoC 实现，LVGL 默认不参与最小 Kernel/ESP32-S31 构建。
