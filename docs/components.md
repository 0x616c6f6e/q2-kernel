# 组件注册与返回值

所有组件通过 `q2_component_register(kernel, descriptor)` 接入，使用相同 ABI、依赖、生命周期和方法表规则。最小描述符只需 `abi / size / id / start / stop`，其余字段可为零。组件 ID 和能力名称使用小写字母、数字、下划线或点；Lua module 使用单层名称，保留 `os`。

```c
static const q2_component_t component = {
    .abi = Q2_COMPONENT_ABI,
    .size = sizeof(q2_component_t),
    .id = "example",
    .start = example_start,
    .stop = example_stop,
};
```

`start` 必须自行回滚部分初始化，失败组件不会再获得 stop；`stop` 和 `release_app` 必须可重试。`poll`、`release_app`、服务表和方法表均可选。服务通过 `q2_service_get(name, abi, size)` 获取，不跨边界暴露 ESP-IDF、FatFs 或 Lua 内部对象。

## 通用 Lua 方法

方法表条目为 `{module, name, capability, invoke}`，由 runtime 自动导出，无需手写 Lua C API。invoke 接收可信 caller、最多四个参数和一个输出值；参数为 bool、int64 或最多 255 字节无 NUL 文本。输出另支持 Q2_NIL，表示无数据成功，Lua 映射为 true。当前不支持二进制块、浮点、复杂对象或流句柄。

操作步骤：验证参数 → 检查资源所有权 → 执行业务 → 写输出 → 返回 Q2_OK。权限检查在统一入口完成，但组件仍需验证参数、配额和 instance；不能把 Lua 输入拼接成任意文件路径。失败时输出被清空，返回非 NUL 终止的字符串会被转换成组件错误。

## 统一错误协议

所有公开操作和业务回调使用 `q2_result_t`，成功为 0、错误为负数；数据通过 `out_*` 返回，仅成功时读取。不可将 `errno`、`esp_err_t`、`FRESULT` 或 SQLite 返回码直接透传。

| 错误组 | 含义 |
|---|---|
| ARGUMENT / FORMAT | 调用参数错误 / 文档或数据格式错误 |
| MEMORY / LIMIT | 分配失败 / 明确配额或队列限制 |
| STATE / BUSY | 生命周期不允许 / 资源占用或正在回调 |
| EXISTS / NOT_FOUND | 重复注册 / 资源不存在 |
| DEPENDENCY / CYCLE / COMPONENT | 依赖缺失、依赖环、必需组件或输出契约失败 |
| UNSUPPORTED / PERMISSION | 未提供能力或不兼容 ABI / 未授权 |
| IO / TIMEOUT / CANCELLED / RUNTIME | I/O、超时、取消、Lua 执行错误 |

精确数值和稳定名称以 `result.h`、`result.c` 为准。Lua 成功返回数据或 true，失败统一 `nil, {code="permission_denied", number=-9}`。布尔 false 可以是正常结果，如 USB 尚未连接，调用方应检查 `value == nil`，不要用 `if not value` 判断所有 API 的失败。

## 已集成组件的具体范围

| 组件 | 实现 | 当前边界 |
|---|---|---|
| storage.private | LittleFS 2.11.3，临时文件后 rename | UUID 私有目录，32 个键/APP，文本值，不是通用 VFS |
| storage.fatfs | FatFs R0.15，真实 FAT 卷 | 单卷，UUID 目录，16 个键；更新非掉电原子 |
| storage.database | SQLite 3.50.4，参数绑定 | 仅内存数据库，UUID 隔离，16 个键/APP，64 页上限；不开放任意 SQL |
| usb.cdc | TinyUSB 0.18.0 | 单控制器 full-speed CDC 文本收发，单次最多 255 字节 |
| display.text | 可注册文本后端，或 LVGL provider | 单 APP 占用、退出清屏 |
| display.ui | LVGL 9.3.0 可选 provider | 标签/按钮/包内 PNG/背景/尺寸查询/点击事件；不是完整 LVGL 绑定，详见 [图形接口](lvgl-display.md) |

SQLite 的最小 VFS 故意拒绝文件打开，避免依赖宿主 OS；因此重启不持久。后续持久化应增加受控 VFS、锁、同步及掉电测试，不能假装目前已经落到 LittleFS。FatFs 的目录及库有全局状态，当前也不是多卷/多线程配置。
