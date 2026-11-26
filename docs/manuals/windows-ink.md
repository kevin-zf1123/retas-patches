# Windows Ink 笔输入

在 Windows 10 或 11 上，可将 Windows Ink 的笔事件和压力传给 32 位 Stylos。适合笔设备已能通过 Windows Ink 正常书写的工作站。

1. 在管理器中导入 `Stylos-Windows-Ink-x86.zip` 并应用更改。这会选择 Windows Ink，停用互斥的原生 WinTab 方案。[下载组件](../../packages/Stylos-Windows-Ink-x86.zip)
2. 正常启动 Stylos，打开空白画格并选用支持压感的画笔。在画布上下两端分别落笔，检查纵向位置，再从轻到重画一条连续线，检查笔迹是否随压力变化。
3. 如果笔能移动但笔压恒定，先在另一款支持 Windows Ink 的绘图软件中用同一支笔测试，再检查设备驱动里的 Windows Ink 设置。

要改用厂商 WinTab 驱动，在管理器中启用[原生数位板坐标修正](tablet-native.md)，应用后重开 Stylos。两种方案由管理器切换，共用安装目录中的 `wintab32.dll`。
