# Windows Ink 输入桥

适用 Windows 10/11 上的 x86 Stylos 2.6.0。把系统笔事件及压感转换成 Stylos 的 WinTab 数据，包含连续采样和纵轴修正。它是原生 WinTab 方案的替代选项，两者都使用安装目录的 wintab32.dll。

从管理器导入 Stylos-Windows-Ink-x86.zip，应用后重开 Stylos，在空白画格检查落点和轻重笔压。停用或切换输入方案也由管理器处理。可用本机 .NET Framework 4.x 的 32 位 csc 和 ilasm 执行 Build.ps1 重新构建运行库。

本包不包含厂商驱动。实体硬件表现取决于驱动和 Windows Ink 设置。
