# Stylos 插件管理器

解压后双击 StylosPluginManager.exe，选择本机的 Stylos.exe。首次安装前请保存工作并退出 Stylos。管理器随后可从 **编辑 → 设置 → 插件管理器…** 打开；同级还有原有的“环境设置…”。默认快捷键为 Ctrl+Shift+P（P 被 Stylos 占用时尝试 J、M），可在原生“快捷键”设置中修改。

**从安装包安装…** 支持本次发行注册表中的组件 ZIP。先解压最外层合集，可一次选取多个子包。列表显示已安装、启用及待应用的变化。停用保留安装记录，可重新启用；卸载清除组件的安装记录和启用状态。原生 WinTab 和 Windows Ink 互斥。旋转和滚轮作为两个插件分别管理。

**应用更改** 前会校验本机版本、冲突和相关文件。Stylos 正在运行时，管理器会等待这台 Stylos 正常退出再执行；可“取消等待”。画稿不会被强制关闭。**打开设置** 跳转到该插件的设置页。用户的比例和画布选项在停用、卸载后保留。

注册表在 registry.json 中维护插件 ID、状态位、设置入口和冲突；发布时绑定子包 SHA-256。未知 ZIP 和构建不会被执行。文件变更记录和原件备份位于安装目录的 .stylos-plugins。出现中断时，先关闭 Stylos，再打开管理器恢复。请保留这个目录。

## 命令行

```powershell
.\StylosPluginManager.exe --setup D:\RETAS\Stylos
.\StylosPluginManager.exe --list D:\RETAS\Stylos
.\StylosPluginManager.exe --install D:\RETAS\Stylos D:\patches\Stylos-Mousewheel-Zoom-x86.zip
.\StylosPluginManager.exe --disable D:\RETAS\Stylos mousewheel-zoom
.\StylosPluginManager.exe --enable D:\RETAS\Stylos mousewheel-zoom
.\StylosPluginManager.exe --uninstall D:\RETAS\Stylos mousewheel-zoom
```

构建时先运行 build_host_packages.py，指定本地授权使用的 --host 基线、原 v0.1 --inbetween 安装器、--manager-assets 暂存目录与 --out 输出目录。完整管理器需要注册表各基线和相应的动检主程序。准备输入与动检、DPI 子包后运行 build_manager.py --assets 同一暂存目录 --out 同一输出目录，即可生成管理器与视图控件包。DevKit 的公开宿主接口见仓库 retas-devkit/。
