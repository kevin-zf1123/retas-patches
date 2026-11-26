# 原生数位板坐标修正

当笔尖与实际落笔位置有偏移，尤其是使用非 100% 桌面缩放或同时安装绘王、Wacom 驱动时，可以使用原生 WinTab 输入方案。请先安装对应设备的厂商驱动，使系统能够识别数位板。

1. 导入 `Stylos-Native-Tablet-x86.zip`，应用更改并关闭原有 Stylos 窗口。[下载组件](../../packages/Stylos-Native-Tablet-x86.zip)
2. **绘王**使用平常的 Stylos 启动方式，插件读取系统的 32 位 WinTab。**Wacom** 使用 Stylos 安装目录里的 `Stylos-Wacom.cmd` 启动，让该次运行选择 Wacom 驱动。
3. 打开空白画格，在画布中心和四角各点一下，比较笔尖和笔迹位置；再从轻到重画一笔，检查笔压。

这项和 [Windows Ink](windows-ink.md) 共用输入接口，管理器会停用另一项。切换设备或输入方案后请重新启动 Stylos。若滚轮或旋转按键也需要调整，它们各有独立的组件和开关。
