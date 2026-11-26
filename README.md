# retas-patches

# how to install

## download latest release

在 [GitHub Releases 最新发行页](https://github.com/kevin-zf1123/retas-patches/releases/latest) 展开 **Assets**，下载该版本的安装包。

- **管理器和组件 ZIP**：解压 `Stylos-Plugin-Manager-x86.zip`，保存画稿并退出 Stylos，运行 `StylosPluginManager.exe`，选中本机的 `Stylos.exe`。在 **从安装包安装…** 中选取组件 ZIP，点 **应用更改**，再重开 Stylos。[插件安装手册](docs/manuals/README.md#安装和停用)
- **v0.1 独立中割版**：解压该发行页的 `Stylos-Inbetween-x86.zip`，运行 `StylosInbetween.exe`。这个版本提供独立安装器。[v0.1 发行页](https://github.com/kevin-zf1123/retas-patches/releases/tag/v0.1)

## build yourself

从仓库根目录运行 CMake。构建目标按插件 ID 命名，组件 ZIP 供 Windows x86 管理器导入，构建产物放在根目录 `packages/`。该目录由 Git 忽略。

| 构建目标 | 构建环境和输入 |
|---|---|
| `high-dpi`、`anti-alias`、`inbetween`、`rotation-shortcuts`、`mousewheel-zoom` | Windows、macOS 或 Linux，CMake 3.19+ 和 Python 3.8+。这些 ZIP 包含清单和说明，功能实现由管理器提供。 |
| `motion-check` | Windows PowerShell 和 Zig 0.13.0（`zig.exe` 在 PATH 中）。 |
| `tablet-native` | Windows PowerShell 和 32 位 Tiny C Compiler（`tcc.exe` 在 PATH 中）。 |
| `windows-ink` | Windows PowerShell 和 32 位 .NET Framework 4.x 的 `csc.exe`、`ilasm.exe`。 |
| `plugin-manager`、`packages` | Windows，以上全部工具、Python 包 `lief` 和 `pefile`、下述 Stylos 参考文件。两者都会生成管理器和八个配套组件。 |

例如，单独构建滚轮缩放组件会生成 `packages/Stylos-Mousewheel-Zoom-x86.zip`。以下命令使用 Ninja 作为 CMake 生成器。将目标换成 `components` 可构建当前平台支持的全部组件。

```sh
cmake -S . -B build -G Ninja
cmake --build build --target mousewheel-zoom
```

构建全套时，先解压 v0.1 发行包取得 `StylosInbetween.exe`。在本机准备 Stylos 2.6.0 x86 参考镜像，按 `<参考目录>/<变体>/<阶段>.exe` 存放。四个变体为 `chs`、`chs-laa`、`precanvas`、`canvas`，每个变体各需 `base.exe`、`stage1.exe` 和 `main.exe`。文件的 SHA-256 应与对应变体和阶段的登记值一致。[参考镜像登记表](stylos/motion-check/versions.json)

在 Windows PowerShell 中把下例的两个路径换成本机路径。构建完成后，按上面的管理器安装步骤使用 `packages/` 里的九个 ZIP。

```powershell
python -m pip install lief==1.0.0 pefile==2024.8.26
cmake -S . -B build -G Ninja `
  "-DPython3_EXECUTABLE=$(python -c 'import sys; print(sys.executable)')" `
  "-DSTYLOS_HOST_DIR=D:/RETAS/references" `
  "-DSTYLOS_INBETWEEN_EXE=D:/RETAS/v0.1/StylosInbetween.exe"
cmake --build build --target packages --parallel
```

## Stylos 2.6.0 · Windows x86

[八个插件的中文使用手册](docs/manuals/README.md)

| 插件 | 源码 | 组件包 |
|---|---|---|
| 旋转快捷键与预览器登记 | [rotation-shortcuts](better-canvas/rotation-shortcuts/) | `Stylos-Rotation-Shortcuts-x86.zip` |
| 鼠标指针中心滚轮缩放 | [mousewheel-zoom](better-canvas/zoom/mousewheel-zoom/) | `Stylos-Mousewheel-Zoom-x86.zip` |
| 双线性与双立方画布显示 | [anti-alias](better-canvas/zoom/anti-alias/) | `Stylos-Better-Canvas-Display-x86.zip` |
| 中割对位 | [rotation-scale-inbetween](stylos/rotation-scale-inbetween/) | `Stylos-Inbetween-x86.zip` |
| 摄影表运动检查、后台预渲染与缓存 | [motion-check](stylos/motion-check/) | `Stylos-Motion-Check-x86.zip` |
| 原生绘王/Wacom 坐标修正 | [tablet-native](stylos/tablet-native/) | `Stylos-Native-Tablet-x86.zip` |
| Windows Ink 笔输入 | [windows-ink](stylos/windows-ink/) | `Stylos-Windows-Ink-x86.zip` |
| 高 DPI 界面 | [high-dpi](stylos/high-dpi/) | `Stylos-High-DPI-x86.zip` |

**环境设置 → 插件** 保留原有插件分类。启用相应组件后，子项“画布显示”和“中割对位”分别显示各自配置。

[RETAS DevKit](retas-devkit/) 维护版本化宿主入口和外部工具接口；[Stylos Plugin Manager](plugin-manager/stylos/) 维护插件注册表、安装包和安装状态。下文是原版中割 v0.1 的介绍。

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
