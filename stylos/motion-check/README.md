# 运动检查、后台预渲染和帧缓存

适用于 Windows x86 Stylos 2.6.0（RETAS STUDIO 6.6.0 中文发行版）。本包合并之前的首轮透明块优化、多线程蒙版合成、空闲预渲染与逐帧无损磁盘缓存。运行时在本机生成两份专用渲染进程；包内不含 Stylos 主程序。此前的性能实验和单阶段安装包已由完整版取代。

将 Stylos-Motion-Check-x86.zip 导入管理器，然后应用更改。启用时管理器按本机基线生成主程序和两个 worker，停用时一并回收 worker。

0.1.1 修正了两个后台 Stylos 实例同时启动时争用临时启动程序的问题。请先升级插件管理器到 0.1.1，然后导入新版动检 ZIP 并应用。A 开始处理第一帧后，动检调度线程才启动 B；此后两个进程仍可并行工作。自动保存可以继续开启。

打开已保存的摄影表，空闲约两秒后后台准备缓存。已保存的画稿变化会使相关画面失效；播放时仍需计算尚未准备好的画面。缓存位于 %LOCALAPPDATA%\StylosMotionRender。前台缓存位图保留目标约 64 MiB，磁盘超过约 512 MiB 时回收至约 448 MiB。

## 维护

src/engine.c 与 scheduler.inc 是运行时源码。motion.asm 是首轮优化的汇编，stubs.asm 记录旧画布基线的渲染接口。plugin.json 声明主程序和 worker 的宿主 RVA 与补丁节。versions.json 保存构建参考镜像使用的历史差分和逐文件哈希。

用 Zig 0.13.0 运行 `Build.ps1` 生成 MotionRender.dll。发布时将生成的 DLL 哈希登记到 versions.json，再将 DLL 通过 `build_manager.py --motion-dll` 传给打包器。Windows 上可用 `python verify_startup.py --zig <zig.exe>` 验证两路并行、短摄影表、保存中断和失败重试。
