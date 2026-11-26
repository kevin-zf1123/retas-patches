# Stylos 2.6.0 画布显示补丁

适用于 Windows x86 的 RETAS STUDIO Stylos 2.6.0 中文构建。双击解压后的 **StylosCanvasSetup.exe**，选择本机安装目录里的 **Stylos.exe**。请先保存画稿并退出该目录下的 Stylos 和后台渲染。安装器需要 .NET Framework 4.5 或以上版本；运行画布模块需要 SSE2。

进入 **编辑 → 设置 → 环境设置**，在左侧分类树下方选择 **原始缩放、双线性、双立方**。默认双线性，双立方适合希望线边更清晰的画稿。确定后刷新画布并保存到当前用户的 CanvasZoom.ini。缩放仅影响预览。两种插值均包含针对纯色线稿区域的优化。

**中割对位 v0.1** 和本补丁可以按任意顺序安装。分类树下的画布选项与底部的“注册中割比…”可以同时使用。数位板驱动、滚轮、快捷键由独立组件管理。

安装器按 SHA-256 识别具体构建并校验输出。原有主程序和需升级的旧滤镜 DLL 各自备份在安装目录，文件名带有 `.before-display-` 和原文件哈希。回退前退出 Stylos，按照安装的逆序还原备份；原先没有滤镜 DLL 时，可在恢复主程序后移走 StylosCanvasFilter.dll。切勿以较早备份覆盖后来安装的其他补丁。

## 从源码构建

需要 Windows、Python 3（pefile 2024.8.26、LIEF 1.0.0）、Zig 0.13.0、.NET Framework csc 和仓库 Release v0.1 的 StylosInbetween.exe。把本机有权使用的基线交给构建器，同一个包可通过重复指定 `--host` 支持多个基线。

```powershell
python better-canvas/zoom/anti-alias/build.py --host D:\RETAS\Stylos\Stylos.exe --inbetween D:\tools\StylosInbetween.exe --out dist
```

ZIP 仅包含离线安装器、说明和许可证。安装器内置经过校验的差分与画布 DLL。命令行安装可运行 `StylosCanvasSetup.exe --install "D:\RETAS\Stylos\Stylos.exe"`。

交付包收录 12 个基线（中文原始版、4 GiB、DPI 修正版、上一版画布及对应的动检阶段），并为每个基线生成中割 v0.1 的共装转换。自建包仅支持构建时提供的基线。若需要动检，请先安装动检，再安装本包和中割。

从仓库根目录可对任一支持的本地主程序运行安装顺序回归检查，脚本使用临时目录：`python better-canvas/zoom/anti-alias/verify.py packages/Stylos-Better-Canvas-Display-x86.zip D:\tools\StylosInbetween.exe D:\RETAS\Stylos\Stylos.exe`。
