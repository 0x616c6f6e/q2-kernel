# 中文字体

`q2-cjk-regular.ttf` 现使用 **LXGW WenKai Lite Regular（霞鹜文楷轻便版）v1.522**。
固定上游提交为 `067a6d508532cc0f8bc79e899ec3b06ff4dee38a`：
[上游项目](https://github.com/lxgw/LxgwWenKai-Lite)。
保留上游 SIL OFL 1.1 授权和版权声明，见 [OFL.txt](OFL.txt)。
处理后的内部字体名称为 Q2 CJK Regular；文件路径沿用原名，让 PC 和 ESP32-S3 共用相同资源。

## 覆盖与显示

- 保留上游全部 **25,598 个 Unicode 码点**，包括简繁汉字、粤语用字、英文与标点。
- 仅去除 Tiny TTF 不使用的排版特性和 hinting；没有按常用字表或 BMP 范围再次裁剪。
- 这不是完整 Unicode 字库；例如上游不含 U+3400，但包含 U+20BB7（𠮷）。
  未收录的歌名字符仍可能显示缺字占位，不会自动向网络下载补字。
- 使用 LVGL Tiny TTF 动态生成 20px 和 40px 字形；缓存分别为 128 和 64 个字形。
- Montserrat 继续负责原有英文、数字及图标；中文等缺失字符回退到霞鹜文楷。
  `q.ui.label/button/set_text/style`、`q.display.text` 和系统 UI 共用字体链。
- 字体嵌入一次，两个字号共享只读数据，不把完整字体复制进 RAM。
- 字体支持与 Music 歌词开关独立；歌词保持关闭，中文歌名照常显示。

## 大小

当前资产为 **13,462,252 字节，约 12.84 MiB**。
旧 Noto 子集为 9,900,208 字节；本次增加 3,562,044 字节（约 3.40 MiB）。
“Lite”是相对该字体完整版而言，不代表比旧 Noto 子集更小。
完整字集优先保障动态歌名；若后续需要减小 Flash 占用，应单独确定裁剪字表。

## 构建与重新生成

正常构建仅使用 Python 标准库，将校验后的本地 TTF 转成构建目录的 C 数组；
无需联网或安装系统字体。上游地址、版本、资源和许可证 SHA-256 均记录在
[manifest.json](manifest.json)，由构建和契约检查验证。

重新生成需要 `fonttools==4.60.1`，下载 manifest 中固定 URL 对应的字体和许可证后执行：

```sh
python scripts/prepare_chinese_font.py /path/to/LXGWWenKaiLite-Regular.ttf --license /path/to/OFL.txt
python scripts/check_contracts.py
```

生成器验证上游两个文件的哈希和 fonttools 版本，并检查处理前后的 Unicode cmap 完全一致。
该字体由 Fontworks 的 Klee 衍生，上游版权与许可证保留；不改变 Q2 源码许可证。


## 可选编译

`Q2_ENABLE_CJK_FONT` 默认 ON；OFF 时不生成/链接字体 C 数组，同时关闭 Tiny TTF，
20/40px 保留 Montserrat 英文、数字和图标，中文不再可用。
完整产品通过根配置 `display.cjk_font` 设置，ESP32-S3 使用 `q2.esp32s3.json`；
独立 CMake 构建可传 `-DQ2_ENABLE_CJK_FONT=OFF`。更改后需重新编译。
