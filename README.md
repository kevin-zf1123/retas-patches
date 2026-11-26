# retas-patches

## Stylos 2.6.0 · Windows x86 补丁下载

| 补丁 | 用途 | 离线安装包 |
|---|---|---|
| [画布显示](better-canvas/zoom/anti-alias/README.md) | 环境设置中的原始、双线性、双立方缩放 | [下载画布补丁](packages/Stylos-Better-Canvas-Display-x86.zip) |
| [中割对位](stylos/rotation-scale-inbetween/README.zh-CN.md) | 旋转、缩放、平移对位与四点预览 | [下载 v0.1](https://github.com/kevin-zf1123/retas-patches/releases/download/v0.1/Stylos-Inbetween-x86.zip) |
| [运动检查](stylos/motion-check/README.md) | 首轮合成、并行预渲染、增量缓存 | [下载完整动检补丁](packages/Stylos-Motion-Check-x86.zip) |
| [原生数位板与视图快捷键](stylos/tablet-native/README.md) | 绘王/Wacom 坐标、指针中心缩放、5° 旋转、登记多张到透光台 | [下载原生 WinTab 包](packages/Stylos-Native-Tablet-x86.zip) |
| [Windows Ink](stylos/windows-ink/README.md) | 从系统笔事件读取笔压 | [下载 Ink 包](packages/Stylos-Windows-Ink-x86.zip) |
| [高 DPI 界面](stylos/high-dpi/README.md) | 让 Windows 放大老版界面 | [下载 DPI 补丁](packages/Stylos-High-DPI-x86.zip) |

画布相关源码按 [Better Canvas](better-canvas/) 下的旋转、滚轮与显示插值分类。只需要改善线条显示时，安装画布补丁即可。画布与中割 v0.1 的安装顺序任选；如需动检，请先安装动检。可选的 DPI 清单补丁最先安装。原生 WinTab 和 Ink 共用同名输入模块，请选择一种。安装器按具体构建校验，其他语言、版本及未经验证的公司自定义版请先核对各组件说明。

## Rotation and scale inbetween alignment

**Stylos 旋转缩放中割对位 · Rotation and scale inbetween alignment · Stylos 回転・拡縮を伴う中割りの位置合わせ**

[下载 v0.1 / Download v0.1 / v0.1 をダウンロード](https://github.com/kevin-zf1123/retas-patches/releases/download/v0.1/Stylos-Inbetween-x86.zip)

## 中文

这个补丁扩展了 32 位 Windows 版 RETAS STUDIO Stylos 2.6.0 的中割对位功能。两张原画在透光台上对齐后，补丁会按指定中割比分配它们之间的旋转、缩放和平移，让两张原画在中割位置重合，当前中割画纸保持原位。可以沿用 Stylos 原有工具手动对齐，也可以在两张原画上各点两个对应特征，快速完成对齐。

关闭 Stylos，先备份本机的 `Stylos.exe`。解压下载的 ZIP，运行 `StylosInbetween.exe`，选中原安装目录的主程序。安装后继续用原来的快捷方式启动。需要 .NET Framework 4.5 或更高版本。[中文使用与构建说明](stylos/rotation-scale-inbetween/README.zh-CN.md)

## English

This patch extends inbetween alignment in RETAS STUDIO Stylos 2.6.0 for 32-bit Windows. Once the two key drawings are aligned on the light table, the patch distributes their relative rotation, scale and translation according to the chosen inbetween fraction. Both references meet at the inbetween position while the active drawing stays in place. Align the references manually with the existing light-table tools, or use the four-point shortcut by picking the same two features on each drawing.

Close Stylos and back up your installed `Stylos.exe`. Extract the ZIP, run `StylosInbetween.exe`, and select that executable. You can keep using your existing launcher. Installation requires .NET Framework 4.5 or later. The patch UI is in Simplified Chinese. [English usage and build guide](stylos/rotation-scale-inbetween/README.md)

## 日本語

32 ビット Windows 版 RETAS STUDIO Stylos 2.6.0 の中割りの位置合わせを拡張するパッチです。ライトテーブルで前後の原画を合わせておくと、指定した中割りの比率に応じて回転・拡縮・移動を両方の原画に振り分けます。動画用紙を動かさずに、中割りの位置で前後の原画を重ねられます。原画同士は従来のツールで手動で合わせることも、対応する特徴を各原画で 2 点ずつ指定してすばやく合わせることもできます。

Stylos を終了し、使用中の `Stylos.exe` をバックアップしてください。ZIP を展開して `StylosInbetween.exe` を起動し、インストール先の `Stylos.exe` を選びます。適用後も従来のショートカットを使えます。.NET Framework 4.5 以降が必要です。パッチの画面表示は簡体字中国語です。[日本語の使い方とビルド手順](stylos/rotation-scale-inbetween/README.ja.md)

## 许可证 / License / ライセンス

[BSD 3-Clause](LICENSE) · 补丁源码与安装器 / Patch source and installer / パッチのソースコードとインストーラ
