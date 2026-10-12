# 命名按键事件透传

本组件只注册名称、校验/转发已识别事件、提供最近事件快照和实例订阅。**SoC / 按键驱动负责单击、双击、长按的识别，APP 负责事件对应的动作。** Kernel 不设时间阈值，不消抖、不合并、不补发、不进行播放/返回/关机等映射。

## SoC 接入

```c
q2_keys_t *keys = NULL;
const q2_component_t *component;
// Check every result in production code; abbreviated here for the registration sequence.
q2_keys_create(kernel, &keys);
q2_keys_register(keys, "POWER");
q2_keys_register(keys, "NEXT");
q2_keys_component(keys, &component);
q2_component_register(kernel, component);
q2_kernel_boot(kernel);
// The driver has already recognized this gesture; forward from the Kernel owner.
q2_keys_report(keys, "POWER", Q2_KEY_LONG);
```

名称区分大小写，最长 31 字节，ASCII 字母开头，其后只允许字母、数字、下划线、连字符和点。注册表上限 32，名称复制保存，boot 后不允许修改。名称无内置语义，POWER 与 CUSTOM_1 采用相同路径。

| C 类型 | Lua event.value | 含义 |
|---|---|---|
| Q2_KEY_SINGLE | 1 | 单击 |
| Q2_KEY_DOUBLE | 2 | 双击 |
| Q2_KEY_LONG | 3 | 长按 |

Q2_KEY_NONE=0 仅用于“尚无事件”快照，不允许 report。IRQ/其他线程先进入 SoC 队列，再由 owner 调用 report，不得跨线程调用 Kernel。用户双击是否同时产生单击、长按是否重复等策略均由驱动定义，Kernel 不做推断。

## Lua API

四个方法都需要声明并授权 input.keys。未注册 provider 时模块不存在；optional APP 先用 q.os.capability 查询。

| 方法 | 成功结果 |
|---|---|
| q.keys.count() | 注册数量 |
| q.keys.name(index) | 1 起始序号对应的名称 |
| q.keys.last(name) | 最近报告的 0/1/2/3；0 表示还没有事件 |
| q.keys.watch(enabled) | true，订阅/取消本次实例的事件 |

```lua
local q = require("q2")
local SINGLE, DOUBLE, LONG = 1, 2, 3
return {
    on_start = function()
        local ok, err = q.keys.watch(true)
        if not ok then error(err.code) end
    end,
    on_event = function(event)
        if event.type == "key.action" then
            local name, action = event.text, event.value
            if name == "NEXT" and action == SINGLE then
                -- APP decides what NEXT means here.
            end
        end
    end,
}
```

每个 report 对应已订阅目标的 key.action，重复类型也原样传递。最多 8 个实例独立订阅。未设置路由时分发给订阅实例；产品调用 q2_keys_route(keys, lifecycle, foreground) 后，只投递 lifecycle，由它判断吞掉或转发。APP 退出/出错释放实例时自动取消订阅；队列中的旧 instance 事件不会交给下一 APP。无订阅者时只更新快照，不在未来订阅时重放。

队列满时 report 返回 Q2_ERR_LIMIT，最近事件快照仍更新，但该事件未入队；驱动应记录/处理失败，不能把快照当作可靠事件日志。只有快照无法恢复丢失次数或顺序。输入消费权限由通用 dispatcher 验证，普通 APP 没有报告/伪造输入的 Lua API。

销毁顺序：runtime → kernel → q2_keys_destroy。x86 Web 服务器先于这些对象关闭；接入不改变最小 port 的三个函数。

## 常驻生命周期路由

`q2_keys_route` 由可信平台 owner 在前台改变后调用。`q.keys.forward(name, action)` 需要 system.lifecycle 且 caller.instance 等于 router instance，只转给当前前台订阅者，不广播给后台。普通 APP 的 watch 不会抢占 Home 的订阅。

锁屏判断在 Homepage Lua：锁定期间吞掉全部动作，仅匹配双击 POWER 时解锁，解锁手势本身不转发。C 层不硬编码 POWER、双击或锁屏业务。平台负责让锁屏期间已识别动作继续到达 router；不能在识别前把所有按键丢掉。

## 通用数据适配

q2_keys_data 在 boot 前注册 input.key.<NAME>.action。q.data.subscribe 需要 data.read 与 input.keys；这里只发布 lifecycle 放行后的前台动作，锁屏原始输入不对普通 APP 广播。managed 模式 q.keys.last 也仅返回调用实例获准接收的动作。完整规则见 [data](data.md)。
