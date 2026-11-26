# 原生数位板、滚轮与视图快捷键

Windows x86 Stylos 2.6.0。绘王使用系统的 32 位 WinTab，Wacom 启动脚本选择厂商的 Wacom_Tablet.dll。这个组件针对双驱动共存和非 100% 桌面缩放时笔尖落点偏离的情况。需要相应厂商驱动。

从管理器导入 Stylos-Native-Tablet-x86.zip，应用后绘王照常启动，Wacom 可运行安装目录中的 Stylos-Wacom.cmd。在管理器中切换此组件与 Windows Ink，两个输入提供者互斥。

滚轮以鼠标指针为中心，每格改变 5 个百分点。工具栏支持左右旋转 5°。快捷键注册中可以重新绑定 `-`、`=` 和文件预览器“登记到透光台”（新配置默认 Ctrl+Shift+L）。已有配置沿用原绑定；若旧版本留下 Ctrl+Alt+L，可在快捷键设置中改为 Ctrl+Shift+L。预览器支持一次选多张。

构建示例：`./Build.ps1 -Tcc C:\tools\tcc\tcc.exe`（32 位 TinyCC 0.9.27）。独立的画布显示补丁负责缩放画质。
