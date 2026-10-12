# 可控日志

Kernel、组件启动/停止、事件队列、授权拒绝和 Lua APP 生命周期/错误通过统一 `q2/log.h` 输出诊断。格式包含时间戳、等级、标签和正文；不会逐帧打印普通轮询。DEBUG 额外记录组件注册和事件投递。

## 两层开关

| 控制 | 用法 | 效果 |
|---|---|---|
| 编译 | CMake `-DQ2_ENABLE_LOG=OFF`，默认 ON | 删除内部日志宏及其参数求值，不编译格式化/输出实现 |
| 运行 | `q2_log_set_level(kernel, Q2_LOG_OFF)` | 暂时静默，之后可重新开启 |
| 等级 | `OFF / ERROR / WARN / INFO / DEBUG` | 输出所选等级及更严重的日志 |

编译关闭时保留 API 桩：get_level 返回 OFF，set_level(OFF) 成功，configure/write 返回 UNSUPPORTED；Lua 的 system.log 不可用。需要支持无日志版本的 APP 应将该能力声明为 optional。

开关针对 Q2 诊断。构建工具、测试结果、命令行参数/启动失败提示、APP 文本显示及 ESP-IDF/bootloader 日志各自独立。

## SoC 注册与 C 调用

```c
#include "q2/log.h"
/* write_log 的签名见 q2_log_sink_t。 */
q2_log_sink_t sink = {Q2_LOG_ABI, sizeof(sink), context, write_log};
q2_result_t result = q2_log_configure(kernel, &sink, Q2_LOG_INFO);
/* 处理 result 后继续启动组件。 */
Q2_LOGI(kernel, "audio", "ready sample_rate=%u", sample_rate);
Q2_LOGE(kernel, "storage", "read failed result=%d", result);
q2_log_set_level(kernel, Q2_LOG_DEBUG);
q2_log_set_level(kernel, Q2_LOG_OFF);
```

新建 Kernel 默认没有 sink，不输出日志；最小 SoC 接入仍只有 allocate/release/now_ms 三个回调。配置按 Kernel 实例保存。configure(kernel, NULL, level) 解除后端。所有操作在 Kernel owner 执行，不从中断或并发线程直接调用。

内部诊断使用 Q2_LOGE/W/I/D 宏。过滤后正文/格式化参数不求值，参数中不要放业务操作。宏忽略 sink 错误，日志故障不改变业务结果。直接调用 q2_log_write 可获取 sink 错误，但参数仍按正常 C 规则先求值。

record 使用栈内存，正文最多 191 字节、标签最多 31 字节；超长正文标记 truncated。时间戳是 SoC 单调时钟毫秒值，不是 UTC。记录仅在回调期间有效，异步后端须复制。sink 不得回调或修改 Kernel；递归 write/configure/set_level/destroy 返回 BUSY。

SoC 参考 console sink 同步写 stderr，将 C0/DEL 控制字符替换为空格，截断加 `[truncated]`。慢速串口可能阻塞 owner，量产高频日志应使用有界异步队列。日志不能包含密码、令牌或完整用户数据。

## PC 与 ESP32

- x86：默认 info，启动参数 `--log-level off|error|warn|info|debug`。
- x86/run.ps1：`-LogLevel debug` 开详细日志，`-LogLevel off` 关闭；`-DisableLog` 编译关闭，后续不带此参数构建会恢复 ON。
- host：环境变量 Q2_LOG_LEVEL，默认 info；PowerShell `$env:Q2_LOG_LEVEL='off'`。文本显示和命令行结果不属于诊断日志。
- ESP32：idf.py menuconfig → Q2 diagnostic logging，CONFIG_Q2_ENABLE_LOG 控制编译，CONFIG_Q2_LOG_LEVEL 为 0–4，默认 3。ESP-IDF/bootloader 日志另行配置。

## Lua APP

manifest 的 optional 能力增加 system.log，宿主显式授权（PC `--grant system.log`）后：

```lua
local q = require("q2")
local available, granted = q.os.capability("system.log")
if available and granted then
    local ok, err = q.os.log("info", "APP ready")
    -- 输出后端也可能失败；不要因诊断失败中止正常业务。
end
```

q.os.log(level, message) 接受 error/warn/info/debug；message 是不含 NUL、最多 255 字节的文本。输出带 APP UUID，最终受 record 的 191 字节上限约束。百分号按字面处理。成功或被过滤返回 true；失败返回 `nil, {code=<错误名>, number=<数值>}`，未授权返回 permission_denied。APP 不能改变宿主日志等级或输出目的地。

## 验证

Q2_BUILD_LOG_TESTS=ON 启用 q2_log_contracts；Q2_BUILD_TESTS 也自动启用。分别用 Q2_ENABLE_LOG=ON/OFF 构建并运行 ctest，覆盖等级过滤、惰性参数、截断、sink 失败、递归保护、实例隔离、Lua 权限及格式串处理。
