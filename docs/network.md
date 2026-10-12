# Network：异步 HTTP / JSON 请求

此组件提供 `network.http`，通过统一组件注册，不增加最小 port 的要求。公开契约为 [network.h](../include/q2/network.h)。Kernel 拥有授权、参数验证、实例隔离、请求配额、截止时间与 Lua API；Platform 注册传输后端。Windows 网络库和线程不进入 Kernel。

## Lua API

APP 的 manifest 声明 `network.http`，产品 grants 同时允许。可声明 optional 并用 `q.os.capability("network.http")` 检查可用且已授权；未注册后端时不报告此能力。

| API | 成功返回 | 说明 |
|---|---|---|
| `q.network.get(url [, timeout_ms])` | 请求整数句柄 | 立即返回；GET；默认 5000 ms，允许 100–10000 ms |
| `q.network.status(id)` | `pending` / `done` | 失败返回 `nil,{code,number}`；不会同步读网络 |
| `q.network.code(id)` | HTTP 状态码 | 完成前 busy；404/500 是 HTTP 响应，不等于传输失败 |
| `q.network.read(id, offset [, count])` | 字符串 | offset 从 0 开始；count 1–255，默认 255；尾部返回空串 |
| `q.network.close(id)` | true | 取消/释放请求，句柄立即失效；完成后也要调用 |

每个 APP 实例最多占用一个 Lua 文本请求和一个原生二进制请求，所有 APP 共四个。未 close 的已完成请求也占用名额。句柄单调增加且不复用，其他 APP 查询/关闭返回 not_found。退出或异常结束由 release_app 自动取消，旧工作线程结果不会投递给新实例。

```lua
local q = require("q2")
local id, next_poll = nil, 0
return {
    on_start = function()
        local available, granted = q.os.capability("network.http")
        if available and granted then
            local err
            id, err = q.network.get("https://example.com/", 5000)
            -- id == nil: handle err.code in the APP UI.
        end
    end,
    on_tick = function(now)
        if not id or now < next_poll then return end
        next_poll = now + 100
        local status, err = q.network.status(id)
        if status == "pending" then return end
        if status == "done" then
            local code = q.network.code(id)
            local first_chunk = q.network.read(id, 0, 240)
            -- Render/store the HTTP code and body as appropriate.
        else
            -- Render err.code, e.g. timeout / io_error / permission_denied.
        end
        q.network.close(id)
        id = nil
    end
}
```

当前使用 on_tick 查询请求状态，不发送完成事件，也不把 HTTP 请求变成 data key。设备状态缓存与网络命令的语义分开。所有 callback 仍为唯一 owner 上的有界调用；同步网络 I/O 不得藏在 get/status/read 中。

## 平台接入

`q2_network_backend_t` 的 ABI、size、context 及 start/poll/release 统一返回 q2_result_t。ABI 2 的 start 复制 URL 并生成任务；poll 在未完成时返回 busy，成功时给出借用响应（有效到 release）。release 必须支持空任务，不等待网络；需要保留的工作线程内存由后端继续持有。组件和后端均需活到 Kernel 停止：runtime → kernel → network component → transport。

Kernel 校验响应状态码 100–599、最大 262144 字节及无 NUL 字节。超大响应 quota_exceeded，含 NUL 的二进制响应 invalid_format；不会把截断的文件当成功下载。read 按字节切片，多字节 UTF-8 可能跨片，使用时先拼接再解码。URL 最多 255 字节；只支持小写 http:// 和 https://、ASCII 或预先百分号编码，不允许凭据、空白、fragment。支持分块构建 JSON POST；尚无任意请求头、自动认证、二进制下载、socket、WebSocket、OTA 集成。

超时以 owner 单调时钟截止时间为准。每次 poll 检查后端并更新结果；失败保留给所属 APP 查询，不终止其他组件。Windows 后端取消后，线程可能仍在当前 WinHTTP 调用中，后台名额需等它退出来才能复用；此期间可能返回 busy，APP 可稍后重试。错误请求也应 close。

## x86 后端

`platform/x86/network_win32.c` 使用 WinHTTP 工作线程，Windows 自己处理 DNS/TCP/TLS，线程只写私有响应。owner 通过已终止的线程句柄读取结果，绝不从线程调用 Kernel 或 Lua。取消只置标记，避免在同步请求正在执行时从另一线程关闭 WinHTTP 句柄。退出进程前最多等 11 秒回收；超时保留任务内存并返回 busy，禁止强杀线程。

使用电脑已有网络，直连（NO_PROXY），不管理 Wi-Fi/BLE。保留系统证书校验；关闭自动重定向、cookie 和认证，30x 返回给 APP。需要企业代理的环境暂不支持代理配置。HTTPS TLS 校验失败映射 permission_denied；DNS 解析失败 not_found，其余网络错误通常为 io_error，超时为 timeout。

微软依据：[WinHTTP 并发规则](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp)、[事务超时](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsettimeouts)、[请求选项](https://learn.microsoft.com/en-us/windows/win32/winhttp/option-flags)。

验证：`q2_network_contracts` 覆盖权限、所有权、配额、超时与清理；x86 的 `test_network.py` 用真实本地 HTTP 服务和真实 Lua 验证请求。Windows 后端不是 ESP 网络驱动，移植后需要重新测试目标 TLS、内存和断网恢复。

## JSON POST 与选择性读取（transport ABI 2）

| API | 说明 |
| --- | --- |
| `q.network.post(url [, timeout_ms])` | 分配草稿请求句柄，尚不发送；默认 5000 ms，允许 100–10000 ms |
| `q.network.write(id, chunk)` | 草稿追加最多 255 字节文本，总体最多 8192 字节；超限失败且保留先前内容 |
| `q.network.send(id)` | 发送 `Content-Type: application/json` 的 POST；截止时间从发送时开始 |
| `q.network.json(id, pointer [, offset])` | 完成响应按 RFC 6901 JSON Pointer 取值；例如 `/data/0/name` |

json 返回布尔、精确整数（绝对值不超过 2^53−1）、字符串的最多 255 字节片段；数组返回长度。
字符串支持从 0 开始的字节 offset，多字节字符可能跨片，应先拼接再显示。
对象和非整数数值返回 unsupported；null/缺失字段返回 not_found。路径区分大小写，数组下标从 0 开始，
`~0`/`~1` 转义波浪号/斜杠。不接受数字下标前导零。空 pointer 指根节点。

JSON 首次按需解析并缓存，沿用固定 cJSON 的 16 层嵌套限制；最大原文 256 KiB，超限由传输返回 quota_exceeded。
语法错误、尾随非空白文本返回 invalid_format。解析树属于请求、位于 C 堆；close/APP 退出释放，
不会把整份歌单复制进 Lua VM。该资源不属于 Lua heap 配额，嵌入式移植须计入平台 RAM 预算。

每 APP 一个、全局四个请求的限制也覆盖草稿。草稿的 status 返回 invalid_state；send 后不可继续 write 或重复 send。
未支持 POST 的 ABI 2 后端把 post 回调设为 NULL，则 Lua post 返回 unsupported。close 可取消任何状态。
ABI 1 后端需更新结构版本到 2，新增可选 post 回调；该回调返回前必须复制 URL、body，不得借用 Lua/组件缓冲。

认证数据由 APP 放在 POST body；不自动共享 cookie、不跟随 30x、不绕过 TLS 校验。URL 和通用 Lua 字符串上限仍为 255。
WinHTTP 工作线程独占请求句柄及其副本，取消/APP 退出后按原有规则回收。不要把 Cookie 放到 URL 或日志。

Music APP 使用这些通用方法；Kernel 没有网易云域名、接口路径、用户字段或登录策略。
Lua manifest 允许的 heap 上限扩展为 384 KiB，以容纳客户端与界面代码；旧 APP 配额保持其原声明值。

## 原生二进制服务

Network 组件注册 `network.binary` ABI 1（`q2_network_binary_t`），供 Display 等原生组件在
owner 线程使用 get/read/close。仍校验调用者的 network.http 授权和实例归属，沿用响应大小、
超时、URL 校验、退出回收规则；read 返回的借用字节仅在 close/release_app 前有效。
二进制不经过 Lua 字符串或 JSON 接口，旧 Lua 网络方法仍拒绝包含 NUL 的文本。
传输 ABI 2 的 read 响应改为允许原始 HTTP 字节，文本限制由 Network 组件施加。
Windows WinHTTP 后端因此可同时下载歌曲封面和请求歌词，而不会把二进制转成文本。
