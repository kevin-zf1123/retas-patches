# retas-patches

# how to install

## download latest release

在 [GitHub Releases 最新发行页](https://github.com/kevin-zf1123/retas-patches/releases/latest) 展开 **Assets**，下载该版本的安装包。

- **管理器和组件 ZIP**：解压 `Stylos-Plugin-Manager-x86.zip`，保存画稿并退出 Stylos，运行 `StylosPluginManager.exe`，选中本机的 `Stylos.exe`。在 **从安装包安装…** 中选取组件 ZIP，点 **应用更改**，再重开 Stylos。[插件安装手册](docs/manuals/README.md#安装和停用)
- **v0.1 独立中割版**：解压该发行页的 `Stylos-Inbetween-x86.zip`，运行 `StylosInbetween.exe`。这个版本提供独立安装器。[v0.1 发行页](https://github.com/kevin-zf1123/retas-patches/releases/tag/v0.1)

## 开放插件系统与构建

0.2 管理器通过界面的 **从安装包安装… → 应用更改** 安装 DevKit ZIP。插件标识、版本、实现与插入声明来自包。未安装插件时列表为空。升级 0.1.x 前请先阅读[迁移说明](plugin-manager/README.md)。

| 目录 | 职责 |
|---|---|
| [plugin-manager](plugin-manager/) | 包管理界面、事务、备份、地址分配和宿主加载器 |
| [retas-devkit](retas-devkit/) | 公开 C ABI、打包工具、宿主适配和独立插件示例 |
| [better-canvas](better-canvas/) 与 [stylos](stylos/) | 各插件的源码、清单和构建配置 |
| [tools/build_plugins.py](tools/build_plugins.py) | 构建本仓库维护的插件；第三方作者可直接使用 DevKit |

Windows 构建需要 Python 3、pefile、LIEF、Zig 0.13、x86 TCC 和 .NET Framework 4.x。管理器只需要两个原始基线 `chs/base.exe`、`chs-laa/base.exe`，独立于插件构建。生成动检、高 DPI 和中割组件还需要按 `<变体>/<阶段>.exe` 放置的参考镜像。动检的 64 位渲染器另需 x64 C++23 工具链和 CMake 3.25 以上。默认 packages 目标只构建 Windows 功能插件。管理器输出到 `packages/0.2.2/`，功能插件位于 `plugins/`，开发工具位于 `devkit/`。

```powershell
cmake -S . -B build -DSTYLOS_HOST_DIR=D:/refs
cmake --build build --target plugin-manager
cmake --build build --target fast-navigation
cmake --build build --target packages
```

## Stylos 2.6.0 · Windows x86

[插件中文使用手册](docs/manuals/README.md)

| 插件 | 源码 | 组件包 |
|---|---|---|
| 快速导航 | [fast-navigation](better-canvas/fast-navigation/) | `Stylos-Fast-Navigation-x86.zip` |
| 旋转快捷键与预览器登记 | [rotation-shortcuts](better-canvas/rotation-shortcuts/) | `Stylos-Rotation-Shortcuts-x86.zip` |
| 鼠标指针中心滚轮缩放 | [mousewheel-zoom](better-canvas/zoom/mousewheel-zoom/) | `Stylos-Mousewheel-Zoom-x86.zip` |
| 双线性与双立方画布显示 | [anti-alias](better-canvas/zoom/anti-alias/) | `Stylos-Better-Canvas-Display-x86.zip` |
| 中割对位 | [rotation-scale-inbetween](stylos/rotation-scale-inbetween/) | `Stylos-Inbetween-x86.zip` |
| 摄影表运动检查、后台预渲染与缓存 | [motion-check](stylos/motion-check/) | `Stylos-Motion-Check-x86.zip` |
| 原生绘王/Wacom 坐标修正 | [tablet-native](stylos/tablet-native/) | `Stylos-Native-Tablet-x86.zip` |
| Windows Ink 笔输入 | [windows-ink](stylos/windows-ink/) | `Stylos-Windows-Ink-x86.zip` |
| 高 DPI 界面 | [high-dpi](stylos/high-dpi/) | `Stylos-High-DPI-x86.zip` |

**环境设置 → 插件** 保留原有插件分类。启用相应组件后，可在相应子项调整画布显示、快速导航、中割对位和动检预渲染。

[RETAS DevKit](retas-devkit/) 维护版本化宿主入口和外部工具接口；[Stylos Plugin Manager](plugin-manager/) 根据已导入插件包维护模块、插入位置和安装状态。下文是原版中割 v0.1 的介绍。

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

[贡献指南 / Contributing / 開発に参加する](CONTRIBUTING.md) · [贡献者 / Contributors / 貢献者](CONTRIBUTORS.md)

[BSD 3-Clause](LICENSE) · 补丁源码与安装器 / Patch source and installer / パッチのソースコードとインストーラ
