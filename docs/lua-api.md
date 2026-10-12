# Lua API 2

图形扩展：可选 LVGL provider 提供 `display.ui` 能力及 `q.ui.label/button/set_text/clear/background/image/image_url/wallpaper/width/height/resolution`，按钮通过 `ui.click` 事件返回句柄。完整规则见 [LVGL Display](lvgl-display.md)，原 display.text/clear 仍兼容。

```lua
local q = require("q2")
return {
    on_start = function()
        local available, granted = q.os.capability("display.text")
        if available and granted then
            local value, err = q.display.text(0, 0, "Hello Q2")
            if value == nil then
                -- Degrade gracefully; do not assume a physical screen exists.
                q.os.exit()
            end
        end
    end,
    on_event = function(event)
        -- event.type / event.value / event.text; only this instance's events arrive.
    end,
}
```

APP 返回生命周期表，`on_start`、`on_event`、`on_tick(now_ms)` 均可选。on_tick 在每次 owner poll 调用，参数为单调毫秒；APP 自行用截止时间限流。最多 8 个独立 VM 协作运行，隐藏 APP 继续 tick；回调仍受指令/时间预算约束，没有后台线程、协程、on_stop 或任意文件模块加载。Q2-App 在构建时把 `src/*.lua` 合并为局部模块。

| 方法 | 能力 | 成功结果 |
|---|---|---|
| `q.os.info()` | system.info | `{name, api, app_id, instance}` |
| `q.os.log(level, message)` | system.log | true；规则见 [日志](logging.md) |
| `q.os.capability(name)` | 无 | `available, granted` |
| `q.os.exit()` | 无 | true，下次 poll 执行退出 |
| `q.storage.get(key)` / `put(key, text)` | storage.private | 文本 / true |
| `q.fatfs.get(key)` / `put(key, text)` | storage.fatfs | 文本 / true |
| `q.sqlite.get(key)` / `put(key, text)` | storage.database | 文本 / true |
| `q.display.text(x, y, text)` / `clear()` | display.text | true |
| `q.usb.connected()` | usb.cdc | bool |
| `q.usb.write_text(text)` | usb.cdc | 已接收发送的字节数，可能小于输入 |
| `q.usb.read_text()` | usb.cdc | 文本；二进制 NUL 会报格式错误 |

组件未 READY 时对应模块可能不存在；先查询 capability，再调用。组件存在但没有授权时返回统一错误。存储键为 1–32 个小写字母、数字、下划线或连字符；不能传 UUID、绝对路径或 `..`。文本值最多 255 字节，读取不存在的键返回 not_found。

## 沙箱与使用约束

只开放经过裁剪的 base/string/table/math/utf8。不开放 io/os/package/debug/coroutine，以及 dofile/loadfile/load/collectgarbage/pcall/xpcall/print/setmetatable/getmetatable。string 的 rep/format/find/match/gmatch/gsub、table 的 sort/move 被移除，以减少不可中断的 C 逻辑。

普通 API 错误通过返回值处理；Lua 语法错误、assert/error、预算超限会终止当前 APP 并清理资源。限制不是硬实时调度，不承诺抵御所有恶意脚本，现阶段仅安装可信开发应用。

系统调用能力不等于授予硬件控制权。manifest 写有 wifi/bt/usb 不会自动创建实现或绕过授权；本版没有无线、音频或下载 API。settings 提供背景选择、系统 UI 配置及偏好保存，不会修改未迁入的硬件。

## 应用目录与返回主页（可选 apps 组件）

| 方法 | 能力 | 成功结果 |
|---|---|---|
| `q.apps.count()` | apps.list | 注册 APP 数量，包含主页 |
| `q.apps.id(index)` | apps.list | 从 1 开始的目录项 UUID |
| `q.apps.info(uuid, field)` | apps.list | name/icon 文本；factory/home/running 布尔值 |
| `q.apps.foreground()` | apps.list | 当前前台 APP 的 UUID |
| `q.apps.last_error()` | apps.list | 最近一次启动/运行失败信息，正常时可为空 |
| `q.apps.launch(uuid)` | apps.launch | true，请求排队成功；不代表目标已运行 |
| `q.apps.home()` | apps.navigate | true，请求显示常驻主页，调用者继续后台运行 |
| `q.appearance.get()` | appearance.read | home-background / midnight / forest |
| `q.appearance.persistent()` | appearance.read | 系统偏好是否具有持久存储 |
| `q.appearance.set(choice)` | appearance.write | true；同时要求产品注册为出厂 APP |

icon 是给主页资源准备使用的别名；调用者无权因查询到名称就读取其他 APP 的资源。业务 APP 可用无额外权限的 q.os.exit() 结束自身，管理模式下自动返回主页；单 APP 调试模式仍结束运行。详情见 [应用管理](application-manager.md)。

## 按键事件

可选 input.keys 能力提供 q.keys.count/name/last/watch。订阅后收到 `{type="key.action", text="按键名称", value=1/2/3}`，分别表示单击、双击、长按。识别由 SoC/驱动完成，功能由 APP 决定。完整生命周期与溢出语义见 [按键透传](keys.md)。

启用系统状态栏时，`q.ui.height()` 返回扣除状态栏后的 APP 区域高度；所有 APP UI 坐标相对该区域。`q.ui.clear()`、背景更新和退出均不删除系统状态栏或屏保。

## 系统层与屏保

| 方法 | 能力 | 成功结果 / 规则 |
|---|---|---|
| `q.shell.get(key)` | system.ui.read | 布尔/整数/文本；配置与状态快照 |
| `q.shell.set(key, value)` | system.settings | true；还须产品认可为 factory，锁定时拒绝 |
| `q.shell.layer(name)` | system.lifecycle | true；仅 resident UUID，home/status/saver |
| `q.shell.present(mode)` | system.lifecycle | true；仅 resident UUID，awake/saver/locked |
| `q.shell.keep_awake()` | screensaver.inhibit | true；仅清醒、未锁定的前台实例，刷新空闲起点 |
| `q.keys.forward(name, action)` | system.lifecycle | true；仅配置的 lifecycle router instance，转给订阅中的前台 |

get/set 配置键：status_bar、screensaver、idle_timeout_ms、clock_move_interval_ms、lock_enabled、status_left、status_right、saver_text、unlock_key、unlock_action。时间 1000–3600000 ms；文本最多 127 UTF-8 字节；解锁动作 1/2/3，名称必须是已注册按键。status_bar_height 仅 get，产品构建时配置。

get 还支持 last_activity（单调 ms）、revision（配置变更计数）、persistent（有无持久存储），以及 time/battery/wifi/bt 文本快照。系统模板由 lifecycle Lua 展开，Kernel 不决定页面业务。锁屏/唤醒事件 `shell.wake` 仅投递 resident；普通 APP 不得据此绕过锁屏。

set 有存储时写入成功才更新内存；无存储仅会话有效。keep_awake 不会关闭屏保配置、唤醒已睡眠 UI 或解锁。必须处理 permission_denied，不能把隐藏 APP 的续期失败视为致命错误。

## 增量图形接口

下列均需 display.ui，只操作 caller 的选中场景。普通 APP 不能选择其他实例或系统层。

| 方法 | 成功结果 |
|---|---|
| `q.ui.scroll(height)` | true；设置纵向内容高度，0 禁用，最大 8192 |
| `q.ui.scroll_y([position])` | 整数滚动位置；可选 position 为 0–8192 |
| `q.ui.position(handle, x, y)` | true；移动自己的控件 |
| `q.ui.size(handle, width, height)` | true；改变控件尺寸，标签超长省略 |
| `q.ui.style(handle, font_size, rgb)` | true；font_size 为 20/40，rgb 为 0xRRGGBB |

状态栏开关改变视口时发送 ui.resize。APP 按新的 q.ui.height 重新布局，不重启 VM。Homepage 自己控制各系统场景，普通 APP 的 clear/退出只影响自身。

## 通用数据订阅

可选 data.read 提供 q.data.get/info/subscribe/unsubscribe；每个 key 还要求注册的领域 capability。状态通知为 data.changed，事件为 data.event，value 支持 bool/int/string，失效时为 nil 并携带 valid/status/sequence/timestamp_ms/subscription。get 只读缓存，事件不可读取历史。详见 [数据契约](data.md)。

可选 `network.http` 提供异步 `q.network.get/post/write/send/status/code/read/json/close`；平台实现真实 HTTP/HTTPS 传输，详见 [Network](network.md)。

`display.ui` 增加 `q.ui.qrcode(x,y,text,size)`，用于把固定文本绘制成二维码；大小含静区，详细限制见 [LVGL Display](lvgl-display.md#二维码)。

可选 `audio.playback` 提供 `q.audio.open/write/play/pause/stop/state/position/duration`。
HTTP/HTTPS 音频还要求 `network.http` 授权；真实播放、独占所有权、分块 URL 和错误约定见 [Audio](audio.md)。

私有 LittleFS 存储每个 APP UUID 最多 32 个键，每个值最多 255 字节；满额时可更新已有键，新增键返回 quota_exceeded。FatFs/SQLite 配额不变。

## 整屏分辨率

`q.ui.resolution()`：需要 `display.ui`，无参数，成功返回 `"502x410"` 一类字符串（配置方向下的宽x高）。状态栏开关、系统图层和 APP 内容区变化不影响该值。失败沿用 nil/error 返回值。用它选择 APP UI profile；具体坐标仍依据 `q.ui.width()/height()`。manifest 可选 `supported_resolutions` 提供 1–16 个唯一尺寸声明，省略兼容旧 APP；声明不会限制启动。C 解析结果保存在 q2_manifest_t.supported_resolutions / supported_resolution_count。

## Lua 执行预算

启动的顶层代码与 on_start 共用约 100000 条 Lua 指令 / 30 ms 执行时间预算；每次 on_tick 和 on_event 重新领取独立预算。每 1000 条指令检查一次，进入注册组件调用前也检查已用时间。

标准库准备和不超过 32 KiB 的文本编译在启动执行计时之外。注册组件方法及日志后端的同步耗时单独排除，返回后接着消耗剩余 Lua 时间；不会恢复已经用掉的时间或指令数，错误返回也遵循同样规则。连续调用 API 的死循环仍被指令限额终止。Lua 参数/结果转换、纯脚本执行及其他标准库工作继续计时。

错误区分 execution budget exceeded (time) 与 (instructions)。这是协作执行保护，不能抢占卡住的原生组件、编译器、GC 或 RTOS 调度；平台仍须限制组件调用耗时并为长 I/O 提供异步实现，不代表取消整个系统的响应时限。
