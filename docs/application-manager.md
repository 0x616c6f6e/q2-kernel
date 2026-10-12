# 常驻主页与多应用管理

可选 `apps` 组件管理可信应用目录、一个可见前台和多个协作运行的 Lua VM。最多 8 个包/运行槽，包含一个出厂 Home。Home 首先启动并常驻；其他 APP 独立拥有 Lua 堆、全局变量、实例 ID、事件与组件资源。

`launch(uuid)` 启动未运行的 APP，或把已运行实例带到前台，不重复执行 on_start。前一个前台转为隐藏，继续 on_tick 和属于自己的事件。`home()` 只显示常驻主页，调用者继续后台运行。`q.os.exit()` 关闭调用者；若其为前台则返回 Home，没有隐式返回栈。默认示例的 Home 按钮采用 exit；想保留业务进程时应改用 home。

## 生命周期与资源所有权

1. 创建 Kernel/provider/manager。注册最多 8 个可信包，UUID 唯一，恰好一个 factory Home。
2. boot 前为显示注册 lifecycle Home UUID 和 factory 设置身份；这些角色不是 manifest 自报字段。
3. boot 后创建一个 caller-owned Lua runtime，`q2_apps_bind` 将它专用于 Home。
4. `q2_apps_request(apps, NULL)` 或指定初始 APP，随后 `q2_apps_advance`。即使初始目标为 Settings，也先启动 Home。
5. 每轮先 `q2_kernel_poll`，在回调外将真实结果交给 `q2_apps_advance`。`q2_apps_state` 返回 home/foreground instance 与运行数；平台据此更新显示 focus 和 keys route。
6. 关闭时先 `q2_apps_shutdown` 停止 Home 并销毁管理器拥有的普通 runtime，再销毁调用者拥有的 Home runtime、Kernel、manager/providers、平台后端。清理失败必须保留对象并重试。

请求只排队，在 owner 回调返回后执行。待处理请求冲突返回 busy。目标/required 权限在切换前验证；普通 APP 故障只结束故障实例，其他 VM 继续运行，前台失败回到 Home 并更新 last_error。Home 故障、清理失败和非 APP 组件 poll 故障向宿主返回错误，不循环重启掩盖失败。

每个目标仍按 manifest ∩ 产品 grants ∩ READY 能力授权。事件按 instance 投递，不是所有 APP 广播。并发是单线程协作调度；回调必须有界，不增加线程/协程或实时抢占保证。

## 资源准备、权限和背景

prepare 回调只在目标需要新启动时执行。必须释放该目标旧场景后才替换它的资源，不能清空其他活跃 APP 的图片。LVGL 使用 `q2_lvgl_clear_app_images(display, uuid)`；稳定图片槽保护仍在使用的描述符。已运行 APP 恢复前台不重新 prepare。

factory/home 由可信平台注册，下载包不能自封。appearance.write 和 system.settings 除 capability 外还检查 factory UUID；system.lifecycle 还检查 resident UUID（按键 forward 检查 router instance）。普通 APP 即使声明这些权限也不能占用系统层。

共享背景提供 home-background / midnight / forest。LittleFS 可用时保存到 Home UUID 的 system-background，失败不更新内存；无存储时仅会话有效。旧 Settings 私有 background 只供 standalone，不覆盖产品偏好。

`q.apps.info(uuid, "running")` 查询运行状态，`q.apps.foreground()` 返回前台 UUID。目录目前启动前固定，没有安装/卸载/OTA。PC 已接入，ESP 最小入口仍未注册产品显示、按键或目录。
