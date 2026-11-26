# Stylos 2.6.0 画布显示

解压并运行 StylosCanvasSetup.exe，选择使用的 32 位 Stylos.exe。先保存画稿并退出该目录的 Stylos 和后台渲染进程。安装器需要 .NET Framework 4.5 或更高版本；渲染模块需要 SSE2。

在 **编辑 → 设置 → 环境设置 → 插件 → 画布显示** 选择原始缩放、双线性或双立方。默认双线性；双立方在线稿边缘更清晰。选择其他分类仍会保留未确认的选项，点击外层“确定”后刷新并记住。画布过滤仅影响显示。与中割补丁同时安装时，同一树下还会显示 **中割对位**；原有“插件”分类仍可打开。

支持通过管理器从本 ZIP 安装、停用、启用或卸载。独立安装也可以运行 StylosCanvasSetup.exe --install "D:\RETAS\Stylos\Stylos.exe"。安装器针对支持的主程序执行差分和哈希校验，保留带 before-display 和原文件哈希的备份。手动还原多个补丁时按逆序恢复对应备份。

构建入口是本目录 build.py。共享的宿主接口定义位于 retas-devkit/，独立包与插件管理器共用 plugin-manager/stylos/build_host_packages.py 生成的兼容矩阵。需要 Zig 0.13.0、Python 3、pefile 2024.8.26、LIEF 1.0.0、本机 .NET Framework csc，以及上游中割 v0.1 安装器。传入一个或多个受支持的本机基线：
```powershell
python better-canvas/zoom/anti-alias/build.py --host D:\RETAS\Stylos\Stylos.exe --inbetween D:\tools\StylosInbetween.exe --out dist
```
