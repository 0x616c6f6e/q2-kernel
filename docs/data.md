# 通用设备数据：缓存状态与事件

可选 data 组件复用 q2_value_t，让 Platform/系统组件通过名称注册数据。Kernel 不列举电池、Wi-Fi 等产品字段，也不调用设备采样函数。APP 使用同一组 Lua 接口读取/订阅；新增同类型数据只扩展 provider 的注册声明、发布逻辑和产品权限配置。

## 契约

注册时复制 name、capability、unit、kind、type、范围和 TTL。名称最多 63 ASCII 字节，首字符为字母，其余字母/数字/下划线/点/连字符；大小写敏感。建议点分命名，兼容原有 POWER 大写按键名。每个名称全局唯一，一个可信 C owner 独占发布权，注册返回句柄。类型固定为 boolean/integer/string，字符串最多 255 UTF-8 字节，无嵌入 NUL。整数量采用明确单位，例如 mV、dBm，无需为每个数据创建结构体或使用 JSON。

- **state**：缓存最新值；相同有效值只更新时间，不重复通知。首次尚未采样为 invalid_state，采样失败可发布 io_error/unsupported 等状态。TTL 到达变为 timeout；不把 0 或 false 当作未知。
- **event**：每次发布都是新事件，即使值相同。没有快照/get/历史重放，也没有去抖或业务映射。foreground_only 可限制只投递当前前台实例。

时间戳是 Platform port 的单调毫秒，表示最近一次采样/显式失效的时间；TTL 失效保留最后采样时间。sequence 在值/有效性变化或事件发生时递增，相同采样刷新不递增。时间/序号按 Kernel 当前运行期解释，重启不持久。

## Lua API 2

| 方法 | 成功返回 | 规则 |
|---|---|---|
| q.data.get(key) | 原生布尔/整数/字符串 | 只读状态缓存，不触发硬件 I/O；event 返回 unsupported |
| q.data.info(key, field) | 文本/整数 | field 为 kind/type/unit/capability；state 还支持 status/sequence/timestamp_ms |
| q.data.subscribe(key [, initial [, min_interval_ms]]) | 订阅句柄 | state 默认初始快照；initial=false 只等待后续变化；interval=0..3600000 |
| q.data.unsubscribe(handle) | true | 只取消本实例订阅，同时删除其尚未投递的通知 |

四个方法都要求 data.read；前三个还校验该 key 注册的 capability，如 power.read、wireless.read。只知道名称不能绕过权限。缺少名称为 not_found，权限不足为 permission_denied；get 的失效状态通过 nil,{code,number} 返回。info(status) 可查询失败原因本身。

订阅同一实例/同一 key 两次返回 already_exists。事件忽略 initial，且不接受非零 min_interval_ms，避免丢掉连续相同动作。隐藏 APP 可继续接收普通状态，foreground_only 事件不会广播给后台。APP 退出自动取消订阅。

状态回调：

```lua
{
    type = "data.changed",       -- 离散事件为 data.event
    key = "power.battery.percent",
    value = 76,                  -- 失效时 nil；false/0 是有效值
    valid = true,
    status = "ok",
    subscription = 1,
    sequence = 3,
    timestamp_ms = 12345
}
```

复用 APP 的 on_event，不增加原生线程或任意 Lua callback。初始快照在后续 owner poll 投递，并与后续更新使用同一个订阅；极快状态变化允许合并到最新快照，不保证看见每个中间值。通知是拷贝的 sample，在回调中只读，不引用驱动临时内存。

```lua
local q = require("q2")
return {
    on_start = function()
        local value, err = q.data.get("power.battery.percent")
        if value ~= nil then
            -- 绘制 value；0 也是有效电量。
        else
            -- 设备不支持、尚未采样、过期或权限被拒，显示未知状态。
        end
        local id, subscription_error = q.data.subscribe("power.battery.percent", true, 1000)
        if not id then
            -- 处理 subscription_error，不假设所有平台都注册了该 key。
        end
    end,
    on_event = function(e)
        if e.type == "data.changed" and e.key == "power.battery.percent" then
            -- 按 e.valid/e.value 更新 UI。
        end
    end,
}
```

manifest 声明 data.read 和 power.read（可选时先用 os.capability 检查）。此数据读取能力只说明通道已注册，不表示电池/网络驱动已实现或已有有效采样。

## C / Platform 接入

```c
q2_data_t *data = NULL;
const q2_component_t *component = NULL;
unsigned percent_handle = 0;
/* 每步检查返回值；battery_driver 是保持稳定地址的可信 provider 对象。 */
q2_data_create(kernel, &data);
q2_data_spec_t spec = {
    .name = "power.battery.percent", .capability = "power.read", .unit = "percent",
    .kind = Q2_DATA_STATE, .type = Q2_INTEGER,
    .bounded = true, .minimum = 0, .maximum = 100, .ttl_ms = 30000
};
q2_data_register(data, &battery_driver, &spec, &percent_handle);
/* 完成全部注册后交给 Kernel，再 boot。 */
q2_data_component(data, &component);
q2_component_register(kernel, component);
/* boot 后，驱动线程先把采样排到 owner，再调用： */
q2_value_t value = {.kind = Q2_INTEGER, .as.integer = 76};
q2_data_publish(data, &battery_driver, percent_handle, &value, Q2_OK);
/* 采样失败： */
q2_data_publish(data, &battery_driver, percent_handle, NULL, Q2_ERR_IO);
```

owner 身份检查防止误用句柄，不是恶意原生代码的安全隔离。所有 API 仅 owner 线程调用；没有自动轮询硬件、后台线程或持久化。shutdown 顺序为 APP/runtime → Kernel → keys/provider → data。数据对象需比引用它的 provider 活得更久。

## 按键与锁屏

keys 注册名称后、boot 前调用 q2_keys_data(keys, data)，生成 input.key.<NAME>.action，能力 input.keys，整数 1/2/3 对应单击/双击/长按。APP 再声明 data.read 即可订阅。

管理模式路径：Platform 识别 → legacy key.action 给 resident Home → Home 决定吞掉/forward → data.event 给前台订阅者。双击 POWER 解锁动作被 Home 吞掉，因此不发布到 APP 的 data 主题。禁止额外公开可被普通 APP 订阅的原始 GPIO 或 raw-action 主题。

这些 key 是 event：get/status/sequence 历史查询不提供按键历史。旧 q.keys.last 在 managed 模式也只返回本实例订阅后获准接收的动作，不能读到锁定期间的原始动作。q2_data_focus 由可信 owner 随前台切换更新，会取消旧前台尚未处理的受限通知；后台订阅保留但不重放漏掉的事件。

Key Demo 优先使用通用 data 订阅，未启用 data 或按键数超过单 APP 订阅配额时使用 legacy watch。不要同时在业务层处理两种按键来源，否则可能重复响应。

## 容量、排队与演进

首版：64 个 key、128 个订阅、每 APP 最多 16 个订阅、最多 32 个领域能力。队列沿用 Kernel 的 32 个事件容量。state 通知按实例/订阅合并；队列满保留待发状态，下次 poll 公平轮转重试。离散事件不合并，满队列返回 LIMIT，可能已有部分订阅者成功收到；发布者必须处理失败，不能无脑重试整个广播造成重复。

Kernel 的 posted/dropped 是排队尝试诊断；state 重试被拒也增加 dropped，不等于最终状态丢失。订阅 min_interval_ms 限制状态通知频率，变化在期限后发最新值；不影响 get 读取最新缓存。取消、退出及 focus 改变会清掉对应待发通知。

q2_event_t 增加了有界 typed payload/metadata，q2_component_t 增加 READY 的额外 capabilities 表，Lua API 保持 2（增量）。C 调用方必须全部重编译。事件队列与注册表增加了静态/堆内存占用，嵌入式需按预算调整固定限额，不能把 Lua heap 配额当全系统上限。

此组件不承载图像、音频流、大数组、任意嵌套对象或控制指令。Wi-Fi connect/音频 play 等命令仍使用组件 API，状态发布实际结果。更改名称、单位或枚举含义属于数据契约变更；注册表 boot 后固定，首版无热注册/通配符订阅。
