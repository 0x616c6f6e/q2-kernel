# 最小 SoC 接入

完整契约见 `include/q2/port.h`、`kernel.h`。应用入口只需要下面的顺序：

```c
static const q2_port_t port = {
    .abi = Q2_PORT_ABI,
    .size = sizeof(q2_port_t),
    .context = NULL,
    .allocate = board_allocate,
    .release = board_release,
    .now_ms = board_now_ms,
};
q2_kernel_t *kernel = NULL;
q2_result_t result = q2_kernel_create(&port, &kernel);
/* Check result, register optional components, then boot and poll on one owner. */
```

| 回调 | 约束 |
|---|---|
| `allocate(ctx, size)` | 返回满足 `max_align_t` 的内存，失败返回 NULL |
| `release(ctx, ptr)` | 释放由相同 port 分配的内存，允许 NULL |
| `now_ms(ctx)` | 64 位单调毫秒时钟，不使用可校时的墙上时钟 |

这些底层回调因分配器/时钟语义保留指针、void、整数返回值；其余对外操作统一使用 `q2_result_t`。结构体使用 `abi + size + context`；不兼容修改提升 ABI，ABI 不同或 size 太小拒绝接入。

## 按需扩展

| 能力 | SoC 提供 | Kernel 提供 |
|---|---|---|
| LittleFS | 字节偏移 read/program/erase/sync、块几何 | 文件系统、APP 私有键值、错误映射 |
| FatFs | 同一块设备协议，512 字节扇区，单卷 | FAT 文件系统和磁盘 glue |
| Display | 尺寸、clear、UTF-8 text | 方法注册、权限、坐标检查、instance 所有权 |
| LVGL Display（可选替代） | 尺寸、同步 XRGB8888 矩形 flush、输入状态 | LVGL 软件渲染、控件、Lua UI 与点击事件 |
| USB | TinyUSB 兼容 DCD、PHY/中断、板级时序 | TinyUSB device/CDC 栈及 Lua 方法 |
| Network | 异步 start/poll/release 传输后端 | HTTP 请求权限、实例隔离、配额与 Lua API |
| 新增驱动 | 自有上下文及 component 描述符 | 生命周期、依赖与服务查询 |

块设备回调必须验证偏移、长度、边界及硬件擦写对齐；`sync` 成功表示介质提交完成。LittleFS 参数需满足创建函数验证；示例为 4096 字节块、16 字节读写粒度。禁止启动失败后自动格式化真实 Flash/SD；格式化是产品层显式决定。

TinyUSB 上游要求的 DCD 是单独可选边界，不是每个 SoC 的最低接入负担。当前参考包使用 TinyUSB 0.18.0，ESP32-S31 的 DCD 未迁移；不要把 tests/fake_dcd.c 链接到实机。使用 USB 时必须提供合法 VID/PID 并核对实际 MCU 配置，不能使用测试值出货。

## 移植验收

1. 独立最小 Kernel 创建、boot、poll、销毁成功，无外设也可以运行。
2. 所有 SDK 头文件只出现在 SoC 工程，Kernel 公共头文件保持可移植。
3. 中断通过 SoC 队列转交 owner；回调不阻塞、不递归执行内核。
4. 逐个注册设备，验证缺失设备、I/O 失败和清理重试。
5. 将 Q2-App 生成的 manifest 和 bundled Lua 交给 `q2_lua_start()`，授权列表由产品策略生成。
6. 实机补充时钟、堆峰值、栈水位、掉电存储、USB 和音频时序测试。

Q2-Platform 提供可运行的 host 示例和可交叉编译的 ESP32-S31 最小工程。其他 SoC 不需要实现 ESP32 风格的 HAL 大接口。
