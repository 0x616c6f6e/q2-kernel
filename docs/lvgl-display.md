# LVGL Display 组件

固定依赖 **LVGL 9.3.0**，通过 `Q2_WITH_LVGL=ON` 按需启用。LVGL 实现在 Kernel，具体窗口/屏幕只提供 `q2_pixel_backend_t`：ABI、size、context、宽高和同步 flush。不增加最小 SoC port 的必需回调。

## 注册和生命周期

```c
q2_lvgl_t *display = NULL;
const q2_component_t *component = NULL;
q2_result_t result = q2_lvgl_create(kernel, &pixel_backend, &display);
/* Check each result before the next operation. */
result = q2_lvgl_component(display, &component);
result = q2_component_register(kernel, component);
```

这是 `display` 的替代 provider，与原 `q2_display_create()` 二选一，不要同时注册。现有 `display.text` 能力仍然支持，新增 `display.ui`。注册规则、授权和返回值继续使用统一组件框架，APP 不获得 LVGL 指针。

所有 LVGL API、flush、输入和定时器都在 Kernel owner 调用。`q2_kernel_poll()` 推进 LVGL tick 和 timer handler；SoC 从自己的事件循环将每次鼠标/触摸状态变化传给 `q2_lvgl_pointer()`，不从 ISR 或窗口工作线程直接调用。

flush 参数是有效矩形和逐行的 **32-bit 0x00RRGGBB / XRGB8888** 像素；stride 以字节计。必须在返回前复制完毕，不保留指针。同步传输失败也会释放 LVGL 绘制缓冲，错误经 poll/输入/清理返回，避免死等。若要 DMA，需要以后增加明确的异步完成协议。

每进程只允许一个运行中的 provider，拥有独立屏幕、输入设备及按 APP 隔离的场景。句柄单调递增，不在清屏或 APP 切换后复用。退出删除控件、重置输入和背景；刷新失败保留 instance，允许重试清理。先销毁 runtime、kernel，再销毁 provider，最后释放像素后端。

## 系统 UI 与多实例

管理模式在 boot 前用 q2_lvgl_lifecycle 指定 Home UUID：Homepage Lua 决定计时、内容和锁屏动作，provider 只提供隔离场景、层级、权限和状态。Home/status/saver 三层归 resident，普通 APP 各有一个内容场景。固定顺序 home < foreground < saver < status；禁用的层隐藏。

q2_lvgl_system_configure 提供出厂默认视口/计时，system_status 提供可信平台快照；q2_lvgl_factory 注册允许改配置的 factory UUID，q2_lvgl_focus 更新前台 instance。所有调用在 owner 上。未启用 lifecycle 的 standalone 保留原生简易屏保兼容行为。

每场景最多 48 个控件，场景总数 Q2_RUNTIME_MAX+2（8 APP 加 2 系统覆盖层）。句柄全局唯一，实例只能查改自身句柄。清屏/退出只清理该实例，隐藏实例仍可更新自己的 UI。启用状态栏后 q.ui.height 为扣除其高度的内容区；切换开关投递 ui.resize，物理输入由 provider 转换。

q2_lvgl_activity 记录真实输入并定向通知 Home；非锁屏首次唤醒过滤整次手势。锁定时已识别按键需交给 lifecycle router，触摸/滚轮不穿透。q2_lvgl_scroll 为窗口滚轮入口。屏保/锁屏不等于停止 APP 或硬件断电。Lua shell API、权限和范围见 [Lua API](lua-api.md)。

## Lua 接口

需要 `display.ui`：

| API | 成功结果 | 约束 |
|---|---|---|
| `q.ui.label(x, y, text [, align])` | 整数句柄 | APP 区域内像素坐标；按剩余宽度自动换行；align 为 left（默认）/center/right，x=0 时可按全屏宽度居中 |
| `q.ui.button(x, y, text [, width])` | 整数句柄 | 默认 160×48；可选宽度 48–1024，必须完整位于屏幕内 |
| `q.ui.set_text(handle, text)` | true | 标签/按钮文字；旧句柄报 not_found |
| `q.ui.clear()` | true | 删除全部当前控件，句柄全部失效 |
| `q.ui.background(0xRRGGBB)` | true | 设置当前背景色并移除壁纸 |
| `q.ui.width()` / `q.ui.height()` | 整数像素 | 返回 APP 可用尺寸，height 不含启用的状态栏 |
| `q.ui.image(x, y, asset [, size])` | 整数句柄 | 显示当前 APP 已注册的 PNG；可选正整数 size 指定完整位于屏幕内的正方形视口，保持比例居中 CONTAIN；不可对图片调用 set_text |
| `q.ui.qrcode(x, y, text, size)` | 整数句柄 | 1–255 字节非空文本；size 58–512，含白边，必须完整位于画布内 |
| `q.ui.wallpaper(asset)` | true | 背景居中 COVER，保持比例，裁切多余区域；位于按钮下方 |

平台在 boot 前或目标 UUID 的旧场景全部释放后调用 `q2_lvgl_register_png(display, app_id, "assets/name.png", bytes, size)`，压缩数据被复制。图片只对匹配 UUID 的 APP 可见，Lua 不获得文件系统路径读取能力。最多 64 张、每张 256 KiB / 512×512 像素、总计 2097152 像素；错误 PNG 在 boot 或运行中注册时拒绝。基于固定 LVGL 9.3 内置 LodePNG，源码未修改，decoder session 的内部头仅在 provider 实现内使用。

解码发生于 boot，或已启动组件的新资源注册时；必须在 Lua 回调外准备资源。多 APP 使用 q2_lvgl_clear_app_images 按 UUID 清理，其他场景/图片保留；该 UUID 仍有场景则 busy。全局 q2_lvgl_clear_images 要求全部场景释放。stop 释放解码数据，destroy 释放其余资源。clear 删除背景图片和控件，release_app 也重置背景色。替换壁纸不会消耗额外控件槽；普通 image 与标签/按钮共享 48 个槽。image 可点击，ui.click 的 value 为其句柄，text 为空字符串。

点击按钮后 `on_event` 收到 `{type="ui.click", value=handle, text=caption}`，caption 最多 127 字节，逻辑判断应使用 handle。事件按 APP instance 路由；清屏前已排队的事件可能携带旧句柄，APP 应按自己的当前状态忽略它们。

原 `q.display.text(x,y,text)` / `clear()` 需要 `display.text` 权限；相同位置重复 text 会更新原标签，避免刷新循环不断创建控件。两组接口共享屏幕、配额和所有权。文本最多 255 UTF-8 字节，统一返回数据/true 或 `nil,{code,number}`。

补充接口：scroll(height)、scroll_y([position])、position(handle,x,y)、size(handle,w,h)、style(handle,20|40,rgb)。详见 [Lua API](lua-api.md)。滚动内容限制在自己的视口，不能覆盖状态栏。

## 构建和资源

```sh
cmake -S . -B build/lvgl -DQ2_WITH_LVGL=ON -DQ2_BUILD_LVGL_TESTS=ON
cmake --build build/lvgl
ctest --test-dir build/lvgl --output-on-failure
```

配置在 `components/display/lv_conf.h`：单线程软件渲染、16 MiB LVGL 固定内存池（包含 PNG 解码及保留的 ARGB 图像）、32 行绘制缓冲（410 宽时 52,480 字节）、Montserrat 20/40 字体及 LXGW WenKai Lite 中文回退字体。PC 后端另外分配完整 framebuffer（410×502×4 字节）。这些内存不属于 Lua 的 32–384 KiB VM 限额；嵌入式需单独测量和调整，不能照搬 PC 内存预算。

这是受控的小型 UI API，不是完整 LVGL Lua 绑定；提供有限的字体/颜色/尺寸/位置及垂直滚动接口，暂未提供任意 LVGL 控件样式、原生 LVGL 键盘/编码器输入、多显示器或异步 DMA。ESP32-S31 工程仍默认关闭 LVGL，实际屏幕接入应由平台注册像素后端后再验证。

依赖为官方标签源码，未改上游文件，下载 SHA-256 和实际文件锁均已记录。依据：[LVGL 9.3 Display 文档](https://lvgl.io/docs/open/9.3/details/main-modules/display/)、[9.3 源码配置](https://github.com/lvgl/lvgl/blob/v9.3.0/lv_conf_template.h)。

## 二维码

`LV_USE_QRCODE=1` 时，`q.ui.qrcode(x,y,text,size)` 使用固定 LVGL 的 QR 组件生成黑白码，权限仍为 display.ui。size 是包含白色静区的外框尺寸；保留至少四个模块的白边，每模块至少两像素，使用整数缩放。内容较长时需要更大的 size，尺寸不足返回 invalid_argument。接口采用字节编码，UTF-8 内容原样写入；1–255 字节限制来自通用 Lua 字符串契约。

句柄按 APP/场景归属管理，占一个控件名额，ui.clear、APP 退出会连同子 canvas 释放。ui.position 可移动，但不允许移出画布；ui.size 不允许改变二维码尺寸，ui.set_text 不适用于二维码。需要不同内容/尺寸时清屏重建。关闭 LV_USE_QRCODE 时方法返回 unsupported，APP 应显示不可用提示。

首个示例是 Q2-App 的 qrcode 应用，只显示固定 `Q2-Kernel` 字符串，不联网、不提供键盘。验证不仅检查像素，还使用独立 ZXing 解码实际 framebuffer。

## 中文字体

默认内嵌 LXGW WenKai Lite Regular v1.522，保留上游全部 25,598 个 Unicode 码点，
包含简繁中文、粤语用字及标点；并非完整 Unicode 字库。
原有 20/40 字号均自动支持 UTF-8 中文，西文和符号保留 Montserrat，不需要 APP 额外注册或申请权限。
字体来源、OFL 许可证、12.84 MiB 资源大小、可复现生成方式及不支持的范围见
[字体说明](../assets/fonts/README.md)。初始化和缓存销毁由 Display provider 管理。

```lua
local label = assert(q.ui.label(24, 60, "正在播放：我的音乐"))
assert(q.ui.style(label, 20, 0xffffff))
```

构建需要 Python 3 标准库，将已校验的本地 TTF 嵌入可执行文件；正常构建无需网络或 fonttools。
验证命令：`python scripts/test_chinese_ui.py --runner <q2_x86.exe> --artifacts <output>`。

## 网络封面

`q.ui.image_url(handle, url)` 给已有 `q.ui.image` 控件异步加载网络图片，同时需要
`display.ui` 和 `network.http` 授权。返回 true 只表示已接收请求，不代表下载成功。
原图片作为加载中和失败时的占位；成功后保留原位置、等比缩放范围、点击句柄。

```lua
local cover = assert(q.ui.image(24, 62, "assets/icon.png", 160))
local accepted, err = q.ui.image_url(cover, "https://example.com/cover.jpg")
-- 未授权、无网络组件时保留包内图标；异步失败也保留占位。
```

仅支持 PNG 和 baseline JPEG，暂不支持 progressive JPEG、WebP、GIF。
URL 遵循 Network 的 HTTP(S)/255 字节限制；响应最多 256 KiB，图片宽高各最多 512。
解码在 owner poll 中进行，每轮最多一张，不占 Lua 回调指令预算；不是后台解码线程。
下载截止时间为 5 秒，超时、非 2xx、无效格式或超限记录 fallback 日志且不重复请求。

每个实例缓存一个 URL，共四个缓存槽，最多约 4 MiB 常驻像素加临时解码内存。
同一 URL 在 `ui.clear` 后重建控件时复用 RAM 缓存。切换 URL 前先清除旧图片控件；
仍有控件引用旧 URL 时返回 busy，避免释放仍被绘制的像素。切歌取消旧请求，APP 退出释放请求和图片。
失败结果同样缓存；重新进入 APP 可重试。无磁盘缓存，也不把登录 Cookie 传给图片 CDN。

验证：`python ../app/scripts/test_cover_ui.py --runner <q2_x86.exe> --artifacts <output>`。

## 平台分配器

`LV_USE_STDLIB_MALLOC` 可以由消费方编译定义覆盖，默认仍为 PC 的 BUILTIN 16 MiB 池。
ESP32-S3 适配使用 CLIB，由 ESP-IDF 的已配置堆/PSRAM 管理大块图形内存；Kernel 不引入 SDK 头文件。
必须在目标设备核对内存余量和 Flash 字体空间。S3 显示/触摸验证入口不代表网络或音频已经移植。


## 可选编译

`Q2_ENABLE_CJK_FONT` 默认 ON；OFF 时不生成/链接字体 C 数组，同时关闭 Tiny TTF，
20/40px 保留 Montserrat 英文、数字和图标，中文不再可用。
完整产品通过根配置 `display.cjk_font` 设置，ESP32-S3 使用 `q2.esp32s3.json`；
独立 CMake 构建可传 `-DQ2_ENABLE_CJK_FONT=OFF`。更改后需重新编译。

## 整屏尺寸与资源变体

`q.ui.resolution()`（display.ui）返回完整逻辑分辨率字符串，例如 `"448x368"`，独立于 APP 内容区和状态栏。平台使用 `q2_lvgl_resolution(display, &w, &h)` 获取同一尺寸，可在 boot 前调用。平台可选择 `assets/<w>x<h>/name.png` 并按稳定的 `assets/name.png` 注册；Kernel 不读取磁盘、不加载其他分辨率、不放宽路径权限。
