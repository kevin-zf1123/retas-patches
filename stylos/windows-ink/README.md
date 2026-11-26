# Windows Ink 输入桥

适用 Windows 10/11 上的 x86 Stylos 2.6.0。把系统笔事件及压感转换成 Stylos 的 WinTab 数据，包含连续采样和纵轴修正。它是原生 WinTab 方案的替代选项，两者都使用安装目录的 wintab32.dll。

退出 Stylos，备份目录内已有的 wintab32.dll 与 StylosInk.dll。把本包这两个 DLL 放到 Stylos.exe 同目录，重开后在空白画格检查落点和轻重笔压。撤销时恢复安装前的两个文件；原来没有的文件直接移走。使用本机 .NET Framework 4.x 的 32 位 csc 和 ilasm 执行 Build.ps1 可重新构建。

本包不包含厂商驱动。实体硬件表现取决于驱动和 Windows Ink 设置。
