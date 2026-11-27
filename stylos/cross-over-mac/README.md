# macOS 上的 CrossOver 插件

这两个插件用于在 Mac 上通过 CrossOver 运行的 Stylos 2.6.0 x86。保存画稿，在支持这两项组件的 Stylos 插件管理器（0.2.2 起）导入所需 ZIP，退出 Stylos，应用更改后从原来的图标重开。

| 插件 | 适用场景 |
|---|---|
| [工具快捷键](shortcuts/) | 输入法开启，画布或浮动面板上的单字母工具键失效。 |
| [起笔压力回退](pressuredriver/) | 使用现有 WinTab 桥时，第一笔在有效压力到达前按鼠标压力起笔。 |

管理器为两项修复分别分配 `.cxkeys` 和 `.cxpress` 节，可独立启停，也可与运动检查一起使用。启用后的实际地址记录在 Stylos 安装目录的 `.stylos-plugins/layout.json`。

Windows 默认构建不包含这两个包。如需单独生成，使用 CMake 目标 `crossover-shortcuts` 或 `crossover-pressure`。
