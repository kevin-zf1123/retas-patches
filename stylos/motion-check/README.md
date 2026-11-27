# 运动检查与预渲染

适用于 64 位 Windows 或 macOS CrossOver 中运行的 32 位 Stylos 2.6.0。安装管理器后导入 `Stylos-Motion-Check-x86.zip`，保存画稿、退出 Stylos 并应用更改。

打开已保存的摄影表后，`MotionRender.dll` 在空闲时启动一个无界面的 `StylosRender64.exe`。渲染器读取 SXF、MN6、CF06、画稿和动检设置，按配置的线程数合成不同曝光（自动模式最多四个线程），将无损帧写入本机缓存。相同的持帧共用一个合成结果。保存工程或恢复操作时调度器可取消当前工作。前台 Stylos 负责交互、播放和声音，未保存的修改仍使用当前文档。

帧缓存位于 `%LOCALAPPDATA%\StylosMotionRender`，按源文件内容、绘制偏好、实际播放设置和曝光索引匹配。调试时可将环境变量 `STYLOS_RENDER_TRACE` 指向可写日志文件，`start-worker`、`worker-exit`、`worker-hit` 分别表示启动、退出和前台命中预渲染帧。

## 构建

从仓库根目录运行 `cmake --build build --target motion-check`。32 位 DLL 使用 Zig 0.13.0，64 位渲染器可使用 Visual Studio 2022 或 x64 MinGW-w64、CMake 与固定版本的压缩和图片库。设置页、前台运行库和后台渲染器随动检包一起更新。[独立渲染器](worker/README.md)

在 **编辑 → 设置 → 环境设置 → 插件 → 动检预渲染** 调整线程数、空闲秒数、后台内存和磁盘缓存上限。可选择缓存的上级文件夹，程序会在其中使用 `StylosMotionRender` 子目录；设置页同时显示所在磁盘的可用空间。点外层 **确定** 后生效。
