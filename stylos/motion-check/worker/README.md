# StylosRender64

Windows 和 macOS CrossOver 共用一个 Windows x64 控制台程序。管理器把它与 x86 缓存 DLL 安装在 Stylos 同目录，调度器以 `CREATE_NO_WINDOW` 启动。需要独立执行时运行：

```powershell
StylosRender64.exe C:\Cut\Cut.sxf C:\Cache C:\Path\To\Stylos\Preference
```

输入工程按只读方式打开。渲染器冻结曝光表并按不同曝光分配线程，每帧独立读取画稿、计算像素和依赖摘要。在同目录写完临时文件后原子发布曝光映射和帧。前台校验依赖、尺寸、解压长度和像素 SHA-256，再将 BGRX 写入 Stylos 的显示位图。

`rebuild/` 包含从 RETAS_Studio_rebuild 选取的摄影表、CutFolder、画稿解码、渲染及其编译依赖。后台使用 Stylos 2.6 的合成规则，PaintMan 的运算规则由独立的渲染配置保留。

使用 CMake 3.25+ 和 Python 运行 `build.py --build BUILD --out OUTPUT/StylosRender64.exe`。默认使用 Visual Studio 2022 x64。也可将 x64 MinGW-w64 GCC（支持 C++23）和 Ninja 加入 PATH，并设置 `$env:CMAKE_GENERATOR='Ninja'`。脚本在构建目录下载并校验依赖源码。已安装静态 zlib、libpng 和 libjpeg-turbo 时，也可直接用本目录的 CMake 项目构建。[第三方软件声明](THIRD-PARTY.txt)
